// Listener.cpp -- Scheme REPL listener.
// Direct port of pyscheme/Listener.py.
#include "Listener.h"
#include "Debugger.h"
#include "profiler.h"
#include "readline_win.h"
#include "Analyzer.h"
#include "Context.h"
#include "Environment.h"
#include "Expander.h"
#include "Parser.h"
#include "Utils.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <io.h>
#  define IS_STDOUT_TTY() (_isatty(_fileno(stdout)) != 0)
#  define IS_STDIN_TTY()  (_isatty(_fileno(stdin))  != 0)
#else
#  include <unistd.h>
#  define IS_STDOUT_TTY() (isatty(STDOUT_FILENO) != 0)
#  define IS_STDIN_TTY()  (isatty(STDIN_FILENO)  != 0)
#endif

namespace fs = std::filesystem;

// ── Class-level statics ───────────────────────────────────────────────────────
bool Listener::s_rl_initialized = false;
int  Listener::s_history_max    = 500;

// ── Internal helpers ──────────────────────────────────────────────────────────

static std::string _substring(const std::string& s, size_t start, size_t end) {
    if (start >= s.size() || start >= end) return {};
    if (end > s.size()) end = s.size();
    return s.substr(start, end - start);
}

static std::string _rstrip(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && (s[end-1] == ' ' || s[end-1] == '\t' ||
                       s[end-1] == '\r' || s[end-1] == '\n'))
        --end;
    return s.substr(0, end);
}

// Check if s starts with pfx (length plen).
static bool _sw(const std::string& s, const char* pfx, size_t plen) {
    return s.size() >= plen && s.compare(0, plen, pfx, plen) == 0;
}

static std::string _timestamp_iso() {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm_info = {};
#ifdef _WIN32
    localtime_s(&tm_info, &tt);
#else
    localtime_r(&tt, &tm_info);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_info);
    return buf;
}

static std::string _timestamp_file() {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm_info = {};
#ifdef _WIN32
    localtime_s(&tm_info, &tt);
#else
    localtime_r(&tt, &tm_info);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d-%H%M%S", &tm_info);
    return buf;
}

static std::string _expanduser(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
    if (!home) return path;
    return std::string(home) + path.substr(1);
}

static std::string _ljust(const std::string& s, int w) {
    if ((int)s.size() >= w) return s;
    return s + std::string(w - (int)s.size(), ' ');
}

static std::vector<std::string> _split_ws(const std::string& s) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size()) break;
        std::string token;
        if (s[i] == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) ++i;
                token += s[i++];
            }
            if (i < s.size()) ++i;  // consume closing "
        } else {
            size_t j = i;
            while (j < s.size() && s[j] != ' ') ++j;
            token = s.substr(i, j - i);
            i = j;
        }
        parts.push_back(std::move(token));
    }
    return parts;
}

// Internal quit signal raised by _cmd_quit to exit readEvalPrintLoop.
struct _QuitSignal {};

static std::string _hist_file_path() {
    const char* home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
    if (!home) return ".pyscheme_history";
    return std::string(home) + "/.pyscheme_history";
}

static std::string s_hist_file;
static void _write_history_atexit() {
    if (!s_hist_file.empty())
        readline_win_write_history_file(s_hist_file);
}

// Build a joined string from a line list (port of '\n'.join(list)).
static std::string _join_lines(const std::vector<std::string>& lines) {
    std::string result;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) result += '\n';
        result += lines[i];
    }
    return result;
}

// Strip leading and trailing whitespace (port of str.strip()).
static std::string _strip(const std::string& s) {
    size_t s0 = s.find_first_not_of(" \t\r\n");
    if (s0 == std::string::npos) return {};
    size_t s1 = s.find_last_not_of(" \t\r\n");
    return s.substr(s0, s1 - s0 + 1);
}

// ── Static method implementations ─────────────────────────────────────────────

std::string Listener::format_call_stack(const std::vector<ShadowEntry>& call_stack) {
    std::string result;
    bool first = true;
    for (const auto& entry : call_stack) {
        if (!first) result += '\n';
        first = false;
        std::string label = entry.label;
        if (entry.count > 1)
            label += " [x" + std::to_string(entry.count) + "]";
        result += "  at " + format_with_caret(label, entry.src);
    }
    return result;
}

std::string Listener::format_error(const std::exception& exc) {
    auto with_stack = [&](const PositionedSchemeError* p,
                          const std::string& type_name) -> std::string {
        std::string msg = type_name + ": " + p->str();
        if (p->call_stack) {
            auto* stk = static_cast<std::vector<ShadowEntry>*>(p->call_stack);
            if (!stk->empty())
                msg += "\n" + Listener::format_call_stack(*stk);
        }
        return msg;
    };

    // Check most-derived subclasses of SchemeRaised first.
    if (auto* p = dynamic_cast<const SchemeFileError*>(&exc))
        return with_stack(p, "SchemeFileError");
    if (auto* p = dynamic_cast<const SchemeUserError*>(&exc))
        return with_stack(p, "SchemeUserError");
    if (auto* p = dynamic_cast<const SchemeRaised*>(&exc))
        return with_stack(p, "SchemeRaised");

    // PositionedSchemeError subclasses that show type name + call stack.
    if (auto* p = dynamic_cast<const SchemeArityError*>(&exc))
        return with_stack(p, "SchemeArityError");
    if (auto* p = dynamic_cast<const SchemeTypeError*>(&exc))
        return with_stack(p, "SchemeTypeError");

    // PositionedSchemeError subclasses that return str() only.
    if (auto* p = dynamic_cast<const SchemeSyntaxError*>(&exc))   return p->str();
    if (auto* p = dynamic_cast<const SchemeAnalysisError*>(&exc)) return p->str();
    if (auto* p = dynamic_cast<const SchemeUnboundError*>(&exc))  return p->str();
    if (auto* p = dynamic_cast<const SchemeRuntimeError*>(&exc))  return p->str();

    // Listener-level error.
    if (dynamic_cast<const ListenerCommandError*>(&exc))           return exc.what();

    // Out-of-memory: report cleanly rather than as a cryptic internal error.
    if (dynamic_cast<const std::bad_alloc*>(&exc))                 return "out of memory";

    std::string msg = exc.what();
    if (!msg.empty()) return "internal error: " + msg;
    return "internal error";
}

std::string Listener::compute_indent(const std::vector<std::string>& lines) {
    int  depth     = 0;
    bool in_string = false;
    bool escape    = false;
    for (const auto& line : lines) {
        for (char ch : line) {
            if (escape) {
                escape = false;
            } else if (in_string) {
                if      (ch == '\\') escape    = true;
                else if (ch == '"')  in_string = false;
            } else {
                if      (ch == '"')              in_string = true;
                else if (ch == ';')              break;
                else if (ch == '(' || ch == '[') ++depth;
                else if (ch == ')' || ch == ']') { if (depth > 0) --depth; }
            }
        }
    }
    return std::string(depth * 3, ' ');
}

std::vector<Listener::LogEntry> Listener::parse_log(const std::string& text) {
    // Split text into lines keeping line endings (port of splitlines(keepends=True)).
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        cur += c;
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) lines.push_back(cur);

    auto rstrip_eq = [](const std::string& s, const char* expected, size_t elen) -> bool {
        size_t end = s.size();
        while (end > 0 && (s[end-1] == ' ' || s[end-1] == '\t' ||
                           s[end-1] == '\r' || s[end-1] == '\n'))
            --end;
        return end == elen && s.compare(0, end, expected, elen) == 0;
    };

    std::vector<LogEntry> entries;
    int  idx       = 0;
    int  n         = (int)lines.size();
    bool fold_case = false;

    while (idx < n) {
        while (idx < n && !_sw(lines[idx], ">>> ", 4)) {
            if (rstrip_eq(lines[idx], "#!fold-case", 11))    fold_case = true;
            else if (rstrip_eq(lines[idx], "#!no-fold-case", 14)) fold_case = false;
            ++idx;
        }
        if (idx >= n) break;

        LogEntry entry;
        entry.fold_case = fold_case;
        entry.expr = lines[idx].substr(4);
        ++idx;

        while (idx < n && _sw(lines[idx], "... ", 4)) {
            entry.expr += lines[idx].substr(4);
            ++idx;
        }

        // Optional bare '...' line (old-style multi-line marker).
        if (idx < n && rstrip_eq(lines[idx], "...", 3) && !_sw(lines[idx], "... ", 4))
            ++idx;

        while (idx < n) {
            const std::string& line = lines[idx];
            if (_sw(line, "==> ", 4) || rstrip_eq(line, "==>", 3)) break;
            if (_sw(line, "... ", 4) || _sw(line, ">>> ", 4) || _sw(line, "%%% ", 4)) break;
            entry.output += line;
            ++idx;
        }

        if (idx < n && (_sw(lines[idx], "==> ", 4) || rstrip_eq(lines[idx], "==>", 3))) {
            const std::string& line = lines[idx];
            if (line.size() > 4) entry.retval = line.substr(4);
            ++idx;
            while (idx < n) {
                const std::string& ln = lines[idx];
                if (_sw(ln, "==> ", 4) || rstrip_eq(ln, "==>", 3)) break;
                if (_sw(ln, "... ", 4) || _sw(ln, ">>> ", 4) || _sw(ln, "%%% ", 4)) break;
                if (_sw(ln, "#!", 2)) break;  // fold-case directive
                if (!ln.empty() && ln[0] == ';') entry.expr   += ln;
                else                              entry.retval += ln;
                ++idx;
            }
        }

        if (idx < n && _sw(lines[idx], "%%% ", 4)) {
            entry.error = lines[idx].substr(4);
            ++idx;
            while (idx < n && _sw(lines[idx], "%%% ", 4)) {
                entry.error += lines[idx].substr(4);
                ++idx;
            }
        }

        if (!entry.expr.empty()) {
            entry.output = _rstrip(entry.output);
            entry.retval = _rstrip(entry.retval);
            entry.error  = _rstrip(entry.error);
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

void Listener::print_welcome_banner() {
    bool color = IS_STDOUT_TTY();
    std::string BOLD_GREEN = color ? "\033[1;92m" : "";
    std::string CYAN       = color ? "\033[96m"   : "";
    std::string RESET      = color ? "\033[0m"    : "";
    std::cout << "Enter any expression to have it evaluated by the interpreter.\n";
    std::cout << "Evaluate '" << CYAN << "(help)" << RESET << "' for online help.\n";
    std::cout << "Type  '" << CYAN << "]help" << RESET << "' to list Listener commands.\n";
    std::cout << BOLD_GREEN << "Welcome!" << RESET << '\n';
}

// ── Constructor / destructor ──────────────────────────────────────────────────

Listener::Listener(InterpreterBase*   interp,
                   const std::string& testdir,
                   const std::string& language,
                   const std::string& version,
                   const std::string& author,
                   const std::string& project,
                   const std::string& compliancedir,
                   const std::string& regressiondir,
                   const std::string& runsdir)
    : _interp(interp)
    , _testdir(fs::absolute(fs::path(testdir)).string())
    , _compliancedir(compliancedir.empty() ? "" : fs::absolute(fs::path(compliancedir)).string())
    , _regressiondir(regressiondir.empty() ? "" : fs::absolute(fs::path(regressiondir)).string())
    , _runsdir(runsdir.empty() ? "" : fs::absolute(fs::path(runsdir)).string())
    , _logStream(nullptr)
    , _language(language)
    , _version(version)
    , _author(author)
    , _project(project)
{
    _init_readline();
    _interp->set_debug_input_fn(
        [this](const std::string& p, const std::string& pf) { return _prompt(p, pf); },
        nullptr);

    _commands["help"]     = [this](std::vector<std::string>& a) { _cmd_help(a); };
    _commands["quit"]     = [this](std::vector<std::string>& a) { _cmd_quit(a); };
    _commands["exit"]     = [this](std::vector<std::string>& a) { _cmd_exit(a); };
    _commands["reboot"]   = [this](std::vector<std::string>& a) { _cmd_reboot(a); };
    _commands["readsrc"]  = [this](std::vector<std::string>& a) { _cmd_readsrc(a); };
    _commands["load"]     = [this](std::vector<std::string>& a) { _cmd_load(a); };
    _commands["readlog"]  = [this](std::vector<std::string>& a) { _cmd_readlog(a); };
    _commands["log"]      = [this](std::vector<std::string>& a) { _cmd_log(a); };
    _commands["close"]    = [this](std::vector<std::string>& a) { _cmd_close(a); };
    _commands["resume"]   = [this](std::vector<std::string>& a) { _cmd_resume(a); };
    _commands["test"]     = [this](std::vector<std::string>& a) { _cmd_test(a); };
    _commands["cd"]       = [this](std::vector<std::string>& a) { _cmd_cd(a); };
    _commands["pwd"]      = [this](std::vector<std::string>& a) { _cmd_pwd(a); };
    _commands["lhistory"] = [this](std::vector<std::string>& a) { _cmd_lhistory(a); };
    _commands["debug"]    = [this](std::vector<std::string>& a) { _cmd_debug(a); };
    _commands["profile"]    = [this](std::vector<std::string>& a) { _cmd_profile(a); };
    _commands["compliance"] = [this](std::vector<std::string>& a) { _cmd_compliance(a); };
    _commands["regression"] = [this](std::vector<std::string>& a) { _cmd_regression(a); };

    _help["help"]     = "Usage: ]help [command]\nList every listener command, or show detailed help for one.";
    _help["quit"]     = "Usage: ]quit\nExit the listener.";
    _help["exit"]     = "Usage: ]exit\nExit the listener (same as ]quit).";
    _help["reboot"]   = "Usage: ]reboot\nReset the interpreter to a fresh global environment.  Any\nuser-defined bindings are lost.  Cannot reboot while logging.";
    _help["readsrc"]  = "Usage: ]readsrc <filename>\nRead and evaluate a Scheme source file.";
    _help["load"]     = "Usage: ]load <filename>\nAlias for ]readsrc.  Read and evaluate a Scheme source file.";
    _help["readlog"]  = "Usage: ]readlog <filename> [v|V]\nRead and evaluate a log file without testing.";
    _help["log"]      = "Usage: ]log <filename>\nBegin a new session-log (dribble) file.  Stop with ]close.";
    _help["close"]    = "Usage: ]close\nClose the current logging session.";
    _help["resume"]   = "Usage: ]resume <filename>\nReplay an existing log file to restore its state, then reopen it for append.";
    _help["test"]     = "Usage: ]test [<filename>]\nRun one log file or all *.log files under testing/.";
    _help["cd"]       = "Usage: ]cd <directory>\nChange the process working directory.";
    _help["pwd"]      = "Usage: ]pwd\nPrint the current working directory.";
    _help["lhistory"] = "Usage: ]lhistory [<n>]\nQuery or set the maximum readline history size.";
    _help["debug"]    = "Usage: ]debug\nOpen the interactive debugger.";
    _help["profile"]    = "Usage: ]profile [reset]\nPrint profiling report (call counts + times) and reset counters.\nWith 'reset', reset counters without printing.\n(Requires build with -DPROFILE_COUNTERS.)";
    _help["compliance"] = "Usage: ]compliance [<file.log> | <start> [<end>]]\nRun the R7RS compliance test suite against the configured directory.\n  ]compliance              -- run all tests\n  ]compliance 3            -- run tests with filename >= \"3\"\n  ]compliance 3 4          -- run tests with \"3\" <= filename < \"4\"\n  ]compliance 3.1 Booleans.log  -- run that one file\nFilename comparison is case-insensitive.  The interpreter is rebooted\nbefore each file.  Supports '==> X or ==> Y' alternatives;\n'%%% *' / '%%% %any-error%' require any error to be raised (R7RS\n'an error is signaled'); '%%% %optional-error%' models R7RS\n'it is an error' -- passes whether an error is raised or the form\nreturns (asserts only that evaluation terminates).";
    _help["regression"] = "Usage: ]regression [<file.log> | <start> [<end>]]\nRun the regression test suite (Scheme-observable, non-spec tripwires) against\nthe configured directory.\n  ]regression                  -- run all regression files\n  ]regression 03               -- run files with filename >= \"03\"\n  ]regression 03 06            -- run files with \"03\" <= filename < \"06\"\n  ]regression 03-evaluator.log -- run that one file\nSpec deviations are guarded by ]compliance instead.  Files are grouped by\nsubsystem; see regression-tests/00-conventions.md.  The interpreter is\nrebooted before each file.";

    _banner();
}

Listener::~Listener() {
    if (_logStream) { _logStream->close(); delete _logStream; _logStream = nullptr; }
}

// ── readline setup ────────────────────────────────────────────────────────────

void Listener::_init_readline() {
    if (s_rl_initialized) return;
#ifdef _WIN32
    s_hist_file = _hist_file_path();
    readline_win_read_history_file(s_hist_file);
    readline_win_set_history_length(s_history_max);
    std::atexit(_write_history_atexit);
    s_rl_initialized = true;
#endif
}

// ── I/O helpers ───────────────────────────────────────────────────────────────

bool Listener::_use_color() const { return IS_STDOUT_TTY() && !_output_to_file; }

void Listener::_banner() {
    bool color = _use_color();
    std::string BOLD_WHITE = color ? "\033[1;97m" : "";
    std::string DIM        = color ? "\033[2m"    : "";
    std::string RESET      = color ? "\033[0m"    : "";
    std::cout << BOLD_WHITE << _language << ' ' << _version << " by " << _author << RESET << '\n';
    std::cout << DIM << "Project home " << _project << RESET << '\n';
    std::cout << '\n';
    std::cout << DIM << "- Interpreter Initialized" << RESET << '\n' << std::flush;
    std::cout << DIM << "- Listener Initialized"    << RESET << '\n' << std::flush;
    std::cout << '\n';
    print_welcome_banner();
    std::cout << '\n';
}

void Listener::_writeLn(const std::string& value, std::ostream* file, bool flush) {
    if (_logStream != nullptr)
        writeln_multiFile(value, {file, _logStream}, flush);
    else
        writeln_multiFile(value, {file}, flush);
}

void Listener::_writeResult(const std::string& text) {
    bool color = _use_color();
    std::string GREEN = color ? "\033[92m"   : "";
    std::string BOLD  = color ? "\033[1;97m" : "";
    std::string RESET = color ? "\033[0m"    : "";

    std::vector<std::string> parts;
    size_t pos = 0;
    while (true) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) { parts.push_back(text.substr(pos)); break; }
        parts.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if (parts.empty()) parts.push_back("");

    for (const auto& line : parts) {
        std::string plain   = "==> " + line;
        std::string colored = GREEN + "==>" + RESET + " " + BOLD + line + RESET;
        _writeLn(color ? colored : plain, nullptr, true);
    }
}

void Listener::_writeErrorMsg(const std::string& errMsg) {
    bool color = _use_color();
    std::string RED   = color ? "\033[91m" : "";
    std::string RESET = color ? "\033[0m"  : "";

    std::vector<std::string> parts;
    size_t pos = 0;
    while (true) {
        size_t nl = errMsg.find('\n', pos);
        if (nl == std::string::npos) { parts.push_back(errMsg.substr(pos)); break; }
        parts.push_back(errMsg.substr(pos, nl - pos));
        pos = nl + 1;
    }
    if (parts.empty()) parts.push_back(errMsg);

    for (const auto& line : parts) {
        std::string plain = "%%% " + line;
        _writeLn(color ? RED + plain + RESET : plain, nullptr, true);
    }
}

std::string Listener::_prompt(const std::string& prompt, const std::string& prefill) {
#ifdef _WIN32
    if (s_rl_initialized && IS_STDIN_TTY()) {
        std::string result = readline_win_input_line(prompt, "... ", prefill);
        while (!result.empty() && (result.back() == '\r' || result.back() == '\n'
                                   || result.back() == ' ' || result.back() == '\t'))
            result.pop_back();
        return result;
    }
#endif
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line))
        throw ReadlineEOFError();
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    return line;
}

// ── Main REPL loop ────────────────────────────────────────────────────────────

void Listener::readEvalPrintLoop() {
    std::vector<std::string> inputExprLineList;

    while (true) {
        std::string lineInput;
        try {
            if (inputExprLineList.empty()) {
                lineInput = _prompt(">>> ");
            } else {
                std::string indent = compute_indent(inputExprLineList);
                lineInput = _prompt("... ", indent);
            }
        } catch (ReadlineEOFError&) {
            std::cout << '\n';
            break;
        } catch (ReadlineInterruptError&) {
            std::cout << '\n';
            inputExprLineList.clear();
            continue;
        }

        bool submit = false;
        if (lineInput.empty()) {
            if (!inputExprLineList.empty()) submit = true;
        } else {
            // Super-bracket: trailing ']' closes all open parens, unless the
            // line itself starts a listener command.
            bool is_cmd = (lineInput[0] == ']' && lineInput.size() > 1);
            if (lineInput.back() == ']' && !is_cmd) {
                std::string tentative = _substring(lineInput, 0, lineInput.size() - 1);
                std::string combined;
                if (!tentative.empty()) {
                    if (!inputExprLineList.empty())
                        combined = _join_lines(inputExprLineList) + '\n' + tentative;
                    else
                        combined = tentative;
                } else {
                    combined = _join_lines(inputExprLineList);
                }
                ParenState ps = paren_state(combined);
                bool innermost_bracket = !ps.stack.empty() && ps.stack.back() == '[';
                if (ps.depth > 0 && !ps.in_string && !innermost_bracket) {
                    lineInput = tentative + std::string(ps.depth, ')');
                } else if (lineInput == "]" && ps.depth == 0 && !ps.in_string) {
                    continue;
                }
            }
            // Mirror line into the dribble log (only non-listener-command lines).
            if (_logStream && !lineInput.empty() && lineInput[0] != ']') {
                std::string logPfx = inputExprLineList.empty() ? ">>> " : "... ";
                *_logStream << logPfx << lineInput << '\n';
            }
            inputExprLineList.push_back(lineInput);
            ParenState ps = paren_state(_join_lines(inputExprLineList));
            if (ps.depth <= 0) submit = true;
        }

        if (!submit) continue;

        std::string inputExprStr = _strip(_join_lines(inputExprLineList));
        inputExprLineList.clear();

        if (s_rl_initialized && !inputExprStr.empty())
            readline_win_add_history(inputExprStr);

        if (inputExprStr.empty()) continue;

        try {
            if (inputExprStr[0] == ']') {
                _runListenerCommand(inputExprStr);
            } else {
                std::string result = _interp->eval(inputExprStr);
                _writeResult(result);
            }
        } catch (_QuitSignal&) {
            break;
        } catch (ReadlineInterruptError&) {
            _writeErrorMsg("Interrupted.");
        } catch (std::exception& e) {
            _writeErrorMsg(format_error(e));
        }

        std::cout << '\n';
    }
}

// ── Session-log parser and runner ─────────────────────────────────────────────

void Listener::sessionLog_restore(const std::string& filename, int verbosity) {
    std::ifstream f(filename, std::ios::in);
    if (!f)
        throw ListenerCommandError("File not found: " + filename);
    std::string text((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    f.close();

    auto entries = parse_log(text);
    int k = 0;
    while (k < (int)entries.size()) {
        const std::string& expr = entries[k].expr;
        if (verbosity > 0) {
            std::vector<std::string> exp_lines;
            size_t pos = 0;
            while (true) {
                size_t nl = expr.find('\n', pos);
                if (nl == std::string::npos) { exp_lines.push_back(expr.substr(pos)); break; }
                exp_lines.push_back(expr.substr(pos, nl - pos));
                pos = nl + 1;
            }
            for (int j = 0; j < (int)exp_lines.size(); ++j)
                std::cout << '\n' << (j == 0 ? ">>> " : "... ") << exp_lines[j];
        }
        std::string resultStr;
        std::string eval_expr = (entries[k].fold_case ? "#!fold-case\n" : "") + expr;
        try { resultStr = _interp->eval(eval_expr); }
        catch (...) {}
        if (verbosity >= 3)
            std::cout << "\n==> " << resultStr;
        ++k;
    }
}

// Supports "X or ==> Y" alternates for return values (R7RS compliance tests
// may list multiple valid results).
static bool compliance_match_retval(const std::string& actual,
                                    const std::string& expected) {
    const std::string sep = " or ==> ";
    std::string rem = expected;
    while (true) {
        size_t idx  = rem.find(sep);
        std::string part = (idx == std::string::npos) ? rem : rem.substr(0, idx);
        size_t s = part.find_first_not_of(" \t");
        size_t e = part.find_last_not_of(" \t");
        if (s != std::string::npos) part = part.substr(s, e - s + 1);
        if (actual == part) return true;
        if (idx == std::string::npos) break;
        rem = rem.substr(idx + sep.size());
    }
    return false;
}

TestResult Listener::sessionLog_test(const std::string& filename, int verbosity) {
    std::ifstream f(filename, std::ios::in);
    if (!f)
        throw ListenerCommandError("File not found: " + filename);
    std::string text((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    f.close();

    bool color = _use_color();
    std::string BOLD  = color ? "\033[1;97m" : "";
    std::string GREEN = color ? "\033[92m"   : "";
    std::string RED   = color ? "\033[91m"   : "";
    std::string DIM   = color ? "\033[2m"    : "";
    std::string RESET = color ? "\033[0m"    : "";

    auto entries = parse_log(text);
    int  n_pass  = 0;
    int  n_fail  = 0;

    std::string saved_fallback = get_include_fallback_dir();
    set_include_fallback_dir(fs::absolute(fs::path(filename)).parent_path().string());

    std::cout << '\n';
    std::cout << BOLD << "Test file:" << RESET << " " << filename << '\n';
    std::cout << BOLD << std::string(11 + filename.size(), '-') << RESET << '\n';

    int k = 0;
    while (k < (int)entries.size()) {
        const auto& entry           = entries[k];
        std::string expected_output = entry.output;
        std::string expected_retval = entry.retval;
        std::string expected_error  = entry.error;
        int         i               = k + 1;

        std::string stripped_expr = _strip(entry.expr);
        std::string eval_expr = (entry.fold_case ? "#!fold-case\n" : "") + stripped_expr;

        std::string actual_retval;
        std::string actual_error;
        std::ostringstream out_capture;

        {
            Context* ctx = _interp->get_ctx();
            ctx->timeout_at     = SteadyClock::now() + std::chrono::seconds(120);
            ctx->timeout_active = true;
            try {
                actual_retval = _interp->eval(eval_expr, &out_capture);
            } catch (ReadlineInterruptError&) {
                actual_error = "Interrupted.";
            } catch (std::exception& e) {
                actual_error = format_error(e);
            }
            ctx->timeout_active = false;
        }

        std::string actual_output = _rstrip(out_capture.str());
        expected_output           = _rstrip(expected_output);

        // "%%% *" or "%%% %any-error% <hint>" means any error is acceptable.
        // "%%% %optional-error% <hint>" models R7RS "it is an error" (undefined
        // behavior): the test passes whether an error is signaled OR the form
        // returns normally -- only termination is asserted.  The retval/output
        // checks are bypassed too, since the outcome is unspecified.
        bool optional_error = _sw(expected_error, "%optional-error%", 16);
        bool error_ok, retval_ok, output_ok;
        if (optional_error) {
            error_ok = retval_ok = output_ok = true;
        } else {
            if (expected_error == "*" || _sw(expected_error, "%any-error%", 11))
                error_ok = !actual_error.empty();
            else
                error_ok = (actual_error == expected_error);
            retval_ok = compliance_match_retval(actual_retval, expected_retval);
            output_ok = (actual_output == expected_output);
        }

        std::string label = stripped_expr;
        size_t nl = label.find('\n');
        if (nl != std::string::npos) label = label.substr(0, nl);
        if ((int)label.size() > 56) label = label.substr(0, 53) + "...";

        char lbuf[8];
        std::snprintf(lbuf, sizeof(lbuf), "%3d", i);

        if (retval_ok && error_ok && output_ok) {
            ++n_pass;
            if (verbosity >= 3)
                std::cout << DIM << "  " << lbuf << ". PASS  " << label << RESET << '\n';
        } else {
            ++n_fail;
            std::cout << RED << "  " << lbuf << ". FAIL  " << label << RESET << '\n';
            if (!retval_ok) {
                std::cout << "         expected return: [" << expected_retval << "]\n";
                std::cout << "         actual return:   [" << actual_retval   << "]\n";
            }
            if (!output_ok) {
                std::cout << "         expected output: [" << expected_output << "]\n";
                std::cout << "         actual output:   [" << actual_output   << "]\n";
            }
            if (!error_ok) {
                if (expected_error == "*" || _sw(expected_error, "%any-error%", 11))
                    std::cout << "         expected an error, but none was raised\n";
                else {
                    std::cout << "         expected error:  [" << expected_error << "]\n";
                    std::cout << "         actual error:    [" << actual_error   << "]\n";
                }
            }
        }
        ++k;
    }

    set_include_fallback_dir(saved_fallback);

    int total = n_pass + n_fail;
    std::cout << '\n';
    if (n_fail == 0)
        std::cout << GREEN << total << " TESTS PASSED" << RESET << '\n';
    else
        std::cout << RED << n_fail << " of " << total << " FAILED" << RESET << '\n';
    return TestResult(n_pass, n_fail);
}

// ── Command dispatch ──────────────────────────────────────────────────────────

void Listener::_runListenerCommand(const std::string& source) {
    std::string body  = _substring(source, 1, source.size());
    auto        parts = _split_ws(body);
    if (parts.empty())
        throw ListenerCommandError("expected a command after ']'");
    std::string cmd = parts[0];
    parts.erase(parts.begin());

    auto it = _commands.find(cmd);
    if (it == _commands.end())
        throw ListenerCommandError("Unknown listener command: " + cmd);
    it->second(parts);
}

void Listener::_runTestFiles(const std::vector<std::string>& filenames, const std::string& testDir,
                             const std::string& suite) {
    bool color = _use_color();
    std::string BOLD  = color ? "\033[1;97m" : "";
    std::string GREEN = color ? "\033[92m"   : "";
    std::string RED   = color ? "\033[91m"   : "";
    std::string RESET = color ? "\033[0m"    : "";

    // Prepare a run report file for every run.
    std::ofstream* runFile    = nullptr;
    std::string    runFilename;
    {
        std::string runsDir = !_runsdir.empty()
            ? _runsdir
            : (fs::path(fs::absolute(fs::path(testDir))) / "runs").string();
        try {
            fs::create_directories(runsDir);
            runFilename = (fs::path(runsDir) /
                           (_timestamp_file() + "-" + suite + "-CPPScheme2.run")).string();
            auto* rf = new std::ofstream(runFilename, std::ios::out);
            if (!rf->is_open()) { delete rf; runFile = nullptr; runFilename = ""; }
            else                runFile = rf;
        } catch (...) { runFile = nullptr; runFilename = ""; }
    }

    int grand_pass = 0, grand_fail = 0;
    struct PerFile { std::string name; int p; int f; };
    std::vector<PerFile> per_file;

    std::string savedCwd = fs::current_path().string();
    fs::current_path(fs::absolute(fs::path(testDir)));

    auto run_start = std::chrono::steady_clock::now();
    std::streambuf* original_buf = std::cout.rdbuf();
    try {
        int k = 0;
        while (k < (int)filenames.size()) {
            std::string filename = filenames[k];
            _interp->reboot(nullptr, false);
            std::string base   = fs::path(filename).filename().string();
            std::string padded = _ljust(base, 56);

            std::cout.rdbuf(original_buf);
            std::cout << padded << ' ' << std::flush;

            if (runFile) std::cout.rdbuf(runFile->rdbuf());
            _output_to_file = (runFile != nullptr);
            TestResult r = sessionLog_test(filename, 3);
            _output_to_file = false;
            std::cout.rdbuf(original_buf);

            grand_pass += r.n_pass;
            grand_fail += r.n_fail;
            per_file.push_back({filename, r.n_pass, r.n_fail});

            std::string status;
            if (r.n_fail == 0)
                status = GREEN + std::to_string(r.n_pass) + " passed" + RESET;
            else {
                int tot = r.n_pass + r.n_fail;
                status = RED + std::to_string(r.n_fail) + " of " +
                         std::to_string(tot) + " failed" + RESET;
            }
            std::cout << status << '\n' << std::flush;
            ++k;
        }
    } catch (...) {
        _output_to_file = false;
        std::cout.rdbuf(original_buf);
        if (runFile) { runFile->close(); delete runFile; }
        fs::current_path(savedCwd);
        throw;
    }
    std::cout.rdbuf(original_buf);
    fs::current_path(savedCwd);

    _interp->reboot(nullptr, false);

    // Total wall-clock time for the whole suite run (all files), formatted
    // as HH:MM:SS.ssssss.
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - run_start).count();
    int    _h = static_cast<int>(elapsed / 3600.0);
    int    _m = static_cast<int>((elapsed - _h * 3600) / 60.0);
    double _s = elapsed - _h * 3600 - _m * 60;
    char _ebuf[64];
    std::snprintf(_ebuf, sizeof(_ebuf), "%02d:%02d:%09.6f", _h, _m, _s);
    std::string elapsed_str(_ebuf);

    // Grand-total screen summary (only for multi-file runs).
    if (filenames.size() > 1) {
        std::cout << '\n';
        int total = grand_pass + grand_fail;
        if (grand_fail == 0)
            std::cout << GREEN << "all " << total << " test cases passed across "
                      << filenames.size() << " files" << RESET << '\n';
        else
            std::cout << RED << grand_fail << " of " << total << " tests failed across "
                      << filenames.size() << " files" << RESET << '\n';
        std::cout << BOLD << "Elapsed: " << elapsed_str << RESET << '\n';

        if (runFile) {
            std::vector<std::string> report;
            report.push_back("");
            report.push_back("");
            report.push_back("Test Report");
            report.push_back("===========");
            for (const auto& pf : per_file) {
                std::string sn = fs::path(pf.name).filename().string();
                std::string msg;
                if (pf.f == 0)
                    msg = std::to_string(pf.p) + " TESTS PASSED!";
                else {
                    int tot = pf.p + pf.f;
                    msg = "(" + std::to_string(pf.f) + "/" + std::to_string(tot) + ") Failed.";
                }
                report.push_back(_ljust(sn, 56) + " " + msg);
            }
            report.push_back("");
            report.push_back("Total test files: " + std::to_string(filenames.size()) + ".");
            report.push_back("Total test cases: " + std::to_string(grand_pass + grand_fail) + ".");
            report.push_back(std::string("Elapsed time: ") + elapsed_str);
            for (const auto& ln : report) *runFile << ln << '\n';
            runFile->close();
            delete runFile;
            std::cout << '\n' << "Test output: " << runFilename << '\n';
        }
    } else {
        // Single-file run: still report how long it took.
        std::cout << BOLD << "Elapsed: " << elapsed_str << RESET << '\n';
        if (runFile) {
            *runFile << "\nElapsed time: " << elapsed_str << "\n";
            runFile->close();
            delete runFile;
        }
    }
}

// ── Individual command handlers ───────────────────────────────────────────────

void Listener::_cmd_help(std::vector<std::string>& args) {
    if (!args.empty()) {
        auto it = _help.find(args[0]);
        if (it == _help.end())
            throw ListenerCommandError("No help on \"" + args[0] + "\".");
        std::cout << it->second << '\n';
        return;
    }
    bool color = _use_color();
    std::string BOLD  = color ? "\033[1;97m" : "";
    std::string CYAN  = color ? "\033[96m"   : "";
    std::string RESET = color ? "\033[0m"    : "";

    std::vector<std::string> names;
    for (const auto& [n, fn] : _commands) names.push_back(n);
    std::sort(names.begin(), names.end());

    std::string header = "Listener Commands";
    std::cout << '\n' << BOLD << header << RESET << '\n';
    std::cout << BOLD << std::string(header.size(), '=') << RESET << '\n';
    columnize(names, 69, nullptr, CYAN);
    std::cout << '\n' << "Type ']help <command>' for detailed help on a command.\n";
}

void Listener::_cmd_quit(std::vector<std::string>& args) {
    if (!args.empty()) throw ListenerCommandError("Usage: ]quit");
    if (_logStream != nullptr) {
        std::vector<std::string> empty;
        _cmd_close(empty);
    }
    std::cout << "Bye.\n";
    throw _QuitSignal{};
}

void Listener::_cmd_exit(std::vector<std::string>& args) { _cmd_quit(args); }

void Listener::_cmd_reboot(std::vector<std::string>& args) {
    if (!args.empty()) throw ListenerCommandError("Usage: ]reboot");
    if (_logStream)
        throw ListenerCommandError("Please close the log file before rebooting (]close).");
    bool color = _use_color();
    std::string DIM   = color ? "\033[2m"  : "";
    std::string RESET = color ? "\033[0m"  : "";
    std::cout << DIM << "- Initializing interpreter" << RESET << '\n';
    _interp->reboot();
    std::cout << '\n';
    print_welcome_banner();
    std::cout << '\n';
}

void Listener::_cmd_readsrc(std::vector<std::string>& args) {
    if (args.size() != 1) throw ListenerCommandError("Usage: ]readsrc <filename>");
    std::string filename = args[0];
    try {
        _interp->evalFile(filename);
    } catch (std::exception& e) {
        std::string msg = e.what();
        if (msg.find("No such file") != std::string::npos ||
            msg.find("not found") != std::string::npos    ||
            msg.find("cannot open") != std::string::npos)
            throw ListenerCommandError("File not found: " + filename);
        throw;
    }
    bool color = _use_color();
    std::string GREEN = color ? "\033[92m" : "";
    std::string RESET = color ? "\033[0m"  : "";
    std::cout << GREEN << "Source file read successfully:" << RESET << " " << filename << '\n';
}

void Listener::_cmd_load(std::vector<std::string>& args) { _cmd_readsrc(args); }

void Listener::_cmd_readlog(std::vector<std::string>& args) {
    if (args.size() != 1 && args.size() != 2)
        throw ListenerCommandError("Usage: ]readlog <filename> [v|V]");
    int verbosity = 0;
    if (args.size() == 2) {
        std::string v = args[1];
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return (char)::toupper(c); });
        if (v == "V") verbosity = 3;
    }
    sessionLog_restore(args[0], verbosity);
    bool color = _use_color();
    std::string GREEN = color ? "\033[92m" : "";
    std::string RESET = color ? "\033[0m"  : "";
    std::cout << GREEN << "Log file read successfully:" << RESET << " " << args[0] << '\n';
}

void Listener::_cmd_log(std::vector<std::string>& args) {
    if (args.size() != 1) throw ListenerCommandError("Usage: ]log <filename>");
    if (_logStream != nullptr)
        throw ListenerCommandError("Already logging.  Close the current log first (]close).");
    auto* f = new std::ofstream(args[0], std::ios::out);
    if (!f->is_open()) { delete f; throw ListenerCommandError("Unable to open file for writing."); }
    _logStream = f;
    _writeLn(";;; Dribble started " + _timestamp_iso());
    _writeLn(";;; " + args[0]);
    _writeLn("");
}

void Listener::_cmd_close(std::vector<std::string>& args) {
    if (!args.empty()) throw ListenerCommandError("Usage: ]close");
    if (_logStream == nullptr) throw ListenerCommandError("Not currently logging.");
    _writeLn("");
    _writeLn(";;; Dribble stopped " + _timestamp_iso());
    _logStream->close();
    delete _logStream;
    _logStream = nullptr;
}

void Listener::_cmd_resume(std::vector<std::string>& args) {
    if (_logStream) throw ListenerCommandError("A log file is already open.  Close it first (]close).");
    if (args.size() != 1) throw ListenerCommandError("Usage: ]resume <filename>");
    sessionLog_restore(args[0]);
    auto* f = new std::ofstream(args[0], std::ios::app);
    if (!f->is_open()) { delete f; throw ListenerCommandError("Unable to reopen file for append."); }
    _logStream = f;
    _writeLn("");
    _writeLn(";;; Dribble resumed " + _timestamp_iso());
    _writeLn("");
}

void Listener::_cmd_compliance(std::vector<std::string>& args) {
    if (_logStream) throw ListenerCommandError("Please close the log before running compliance (]close).");

    std::string compdir = _compliancedir;
    if (compdir.empty())
        throw ListenerCommandError(
            "No compliance directory configured.  Set compliancedir at startup.");
    if (!fs::is_directory(compdir))
        throw ListenerCommandError("Compliance directory not found: " + compdir);

    // Detect single-file mode: last token ends with ".log".
    // Tokens are rejoined since filenames may contain spaces.
    bool single_file_mode = !args.empty() &&
        args.back().size() >= 4 &&
        args.back().substr(args.back().size() - 4) == ".log";

    if (single_file_mode) {
        // Rejoin all tokens as the filename (handles spaces in filenames).
        std::string fname;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) fname += ' ';
            fname += args[i];
        }
        std::string fpath = (fs::path(compdir) / fname).string();
        if (!fs::is_regular_file(fpath))
            throw ListenerCommandError("File not found: " + fname);
        _runTestFiles({fpath}, compdir, "compliance");
        return;
    }

    // Range mode: 0 args = all, 1 arg = [start, ∞), 2 args = [start, end).
    if (args.size() > 2)
        throw ListenerCommandError(
            "Usage: ]compliance [<file.log> | <start> [<end>]]");

    std::vector<std::string> all_files = retrieveFileList(compdir);
    if (all_files.empty())
        throw ListenerCommandError("No .log files in " + compdir);

    if (args.empty()) {
        _runTestFiles(all_files, compdir, "compliance");
        return;
    }

    // Case-insensitive filename comparison.
    auto ci_lower = [](const std::string& s) {
        std::string r = s;
        std::transform(r.begin(), r.end(), r.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return r;
    };

    std::string start_lc = ci_lower(args[0]);
    std::string end_lc   = args.size() == 2 ? ci_lower(args[1]) : "";
    bool        has_end  = args.size() == 2;

    std::vector<std::string> filtered;
    for (const std::string& fpath : all_files) {
        std::string fname_lc = ci_lower(fs::path(fpath).filename().string());
        if (fname_lc < start_lc) continue;
        if (has_end && fname_lc >= end_lc) continue;
        filtered.push_back(fpath);
    }

    if (filtered.empty())
        throw ListenerCommandError(
            has_end
            ? "No .log files in range [" + args[0] + ", " + args[1] + ")"
            : "No .log files at or after \"" + args[0] + "\"");

    _runTestFiles(filtered, compdir, "compliance");
}

void Listener::_cmd_regression(std::vector<std::string>& args) {
    if (_logStream) throw ListenerCommandError("Please close the log before running regressions (]close).");

    std::string regdir = _regressiondir;
    if (regdir.empty())
        throw ListenerCommandError(
            "No regression directory configured.  Set regressiondir at startup.");
    if (!fs::is_directory(regdir))
        throw ListenerCommandError("Regression directory not found: " + regdir);

    // Detect single-file mode: last token ends with ".log".
    bool single_file_mode = !args.empty() &&
        args.back().size() >= 4 &&
        args.back().substr(args.back().size() - 4) == ".log";

    if (single_file_mode) {
        std::string fname;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) fname += ' ';
            fname += args[i];
        }
        std::string fpath = (fs::path(regdir) / fname).string();
        if (!fs::is_regular_file(fpath))
            throw ListenerCommandError("File not found: " + fname);
        _runTestFiles({fpath}, regdir, "regression");
        return;
    }

    // Range mode: 0 args = all, 1 arg = [start, inf), 2 args = [start, end).
    if (args.size() > 2)
        throw ListenerCommandError(
            "Usage: ]regression [<file.log> | <start> [<end>]]");

    std::vector<std::string> all_files = retrieveFileList(regdir);
    if (all_files.empty())
        throw ListenerCommandError("No .log files in " + regdir);

    if (args.empty()) {
        _runTestFiles(all_files, regdir, "regression");
        return;
    }

    auto ci_lower = [](const std::string& s) {
        std::string r = s;
        std::transform(r.begin(), r.end(), r.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return r;
    };

    std::string start_lc = ci_lower(args[0]);
    std::string end_lc   = args.size() == 2 ? ci_lower(args[1]) : "";
    bool        has_end  = args.size() == 2;

    std::vector<std::string> filtered;
    for (const std::string& fpath : all_files) {
        std::string fname_lc = ci_lower(fs::path(fpath).filename().string());
        if (fname_lc < start_lc) continue;
        if (has_end && fname_lc >= end_lc) continue;
        filtered.push_back(fpath);
    }

    if (filtered.empty())
        throw ListenerCommandError(
            has_end
            ? "No .log files in range [" + args[0] + ", " + args[1] + ")"
            : "No .log files at or after \"" + args[0] + "\"");

    _runTestFiles(filtered, regdir, "regression");
}

void Listener::_cmd_test(std::vector<std::string>& args) {
    if (args.size() > 1) throw ListenerCommandError("Usage: ]test [<filename>]");
    if (_logStream) throw ListenerCommandError("Please close the log before running tests (]close).");
    std::vector<std::string> filenames;
    std::string testDir;
    if (args.size() == 1) {
        const std::string& arg = args[0];
        if (fs::is_directory(arg)) {
            testDir   = fs::absolute(fs::path(arg)).string();
            filenames = retrieveFileList(arg);
            if (filenames.empty())
                throw ListenerCommandError("No .log files in " + arg);
        } else {
            testDir   = fs::absolute(fs::path(arg)).parent_path().string();
            filenames.push_back(arg);
        }
    } else {
        if (!fs::is_directory(_testdir))
            throw ListenerCommandError("No test directory: " + _testdir);
        testDir   = fs::absolute(fs::path(_testdir)).string();
        filenames = retrieveFileList(_testdir);
        if (filenames.empty())
            throw ListenerCommandError("No .log files in " + _testdir);
    }
    _runTestFiles(filenames, testDir, "feature");
}

void Listener::_cmd_cd(std::vector<std::string>& args) {
    if (args.size() != 1) throw ListenerCommandError("Usage: ]cd <directory>");
    std::string target = _expanduser(args[0]);
    if (!fs::is_directory(target))
        throw ListenerCommandError("Not a directory: " + target);
    fs::current_path(target);
    bool color = _use_color();
    std::string DIM   = color ? "\033[2m"  : "";
    std::string RESET = color ? "\033[0m"  : "";
    std::cout << DIM << fs::current_path().string() << RESET << '\n';
}

void Listener::_cmd_pwd(std::vector<std::string>& args) {
    if (!args.empty()) throw ListenerCommandError("Usage: ]pwd");
    std::cout << fs::current_path().string() << '\n';
}

void Listener::_cmd_lhistory(std::vector<std::string>& args) {
    if (args.size() > 1) throw ListenerCommandError("Usage: ]lhistory [<n>]");
    if (args.empty()) {
        std::cout << "Current history size: " << s_history_max << '\n';
        return;
    }
    int n;
    try { n = std::stoi(args[0]); }
    catch (...) { throw ListenerCommandError("History size must be an integer."); }
    if (n < 1) throw ListenerCommandError("History size must be a positive integer.");
    s_history_max = n;
    if (s_rl_initialized) readline_win_set_history_length(n);
    std::cout << "New history size: " << n << '\n';
}

void Listener::_cmd_debug(std::vector<std::string>& args) {
    if (!args.empty()) throw ListenerCommandError("Usage: ]debug");
    Context* ctx = _interp->get_ctx();
    if (!ctx || !ctx->debugger)
        throw ListenerCommandError("Debugger not active.");
    ctx->debugger->run_debugger_repl(ctx, _interp->get_env());
}

void Listener::_cmd_profile(std::vector<std::string>& args) {
    if (!args.empty() && args[0] == "reset") {
        prof_reset();
        std::cout << "Profile counters reset.\n";
        return;
    }
    if (!args.empty())
        throw ListenerCommandError(
            "Usage: ]profile [reset]\n"
            "  With no arguments, print profiling report and reset counters.\n"
            "  With 'reset', reset counters without printing.\n"
            "  (Requires build with -DPROFILE_COUNTERS.)");
    prof_report();
    prof_reset();
}
