#pragma once
// Listener.h -- Scheme REPL listener.
// Direct port of pyscheme/Listener.py.
#include "AST.h"
#include "scheme_export.h"
#include <exception>
#include <fstream>
#include <functional>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

struct Context;
struct Environment;  // AST.h forward-declares Environment as struct

// ── InterpreterBase ───────────────────────────────────────────────────────────
// Abstract interface that the Listener expects its interpreter to provide.
// Port of Listener.py InterpreterBase.

struct CEKSCHEME_API InterpreterBase {
    virtual ~InterpreterBase() = default;

    // Reset to a fresh global environment.
    // load_rc: when true, load the user RC file after rebooting.
    virtual void reboot(std::ostream* outStrm = nullptr, bool load_rc = true) = 0;

    // Evaluate a source string. Returns the pretty-printed result string.
    // Side-effect output (display, newline, etc.) goes to outStrm if non-null,
    // otherwise to the Context's outStrm.
    virtual std::string eval(const std::string& source,
                             std::ostream* outStrm = nullptr) = 0;

    // Read and evaluate an entire source file.
    virtual void evalFile(const std::string& filename,
                          std::ostream* outStrm = nullptr) = 0;

    // Register the Listener's prompt function for debug REPLs (Phase 17).
    // fn: (prompt, prefill) -> line read; rl: opaque readline handle.
    virtual void set_debug_input_fn(
        std::function<std::string(const std::string&, const std::string&)> fn,
        void* rl = nullptr) {}

    // Access to ctx and env for ]debug (Phase 17).
    virtual Context*     get_ctx() { return nullptr; }
    virtual Environment* get_env() { return nullptr; }
};

// ── ListenerCommandError ──────────────────────────────────────────────────────
// Raised by listener-command bodies to signal a user-level error.
// Port of Listener.py ListenerCommandError.
struct CEKSCHEME_API ListenerCommandError : std::exception {
    explicit ListenerCommandError(const std::string& msg) : _msg(msg) {}
    const char* what() const noexcept override { return _msg.c_str(); }
private:
    std::string _msg;
};

// ── TestResult ────────────────────────────────────────────────────────────────
// Return container for sessionLog_test: pass/fail counts.
// Port of Listener.py TestResult.
struct CEKSCHEME_API TestResult {
    int n_pass;
    int n_fail;
    TestResult(int p, int f) : n_pass(p), n_fail(f) {}
};

// ── Listener ──────────────────────────────────────────────────────────────────
// Interactive REPL with session logging and log-based testing.
// Port of Listener.py Listener.

class CEKSCHEME_API Listener {
public:
    // Each parsed log entry: expression, output, return value, error message.
    struct LogEntry {
        std::string expr;
        std::string output;
        std::string retval;
        std::string error;
    };

    Listener(InterpreterBase*   interp,
             const std::string& testdir       = "testing",
             const std::string& language      = "cekscheme",
             const std::string& version       = "0.1",
             const std::string& author        = "cekscheme authors",
             const std::string& project       = "https://example/cekscheme",
             const std::string& compliancedir = "");
    ~Listener();

    // Run the interactive REPL until EOF or ]quit/]exit.
    void readEvalPrintLoop();

    // Replay a log file without testing (used by ]readlog and ]resume).
    void sessionLog_restore(const std::string& filename, int verbosity = 0);

    // Run a log file through the test harness; return pass/fail counts.
    // Supports "X or ==> Y" alternates in return values and "%%% *" as
    // a wildcard meaning any error is acceptable.
    TestResult sessionLog_test(const std::string& filename, int verbosity = 3);

    // ── Static helpers ─────────────────────────────────────────────────────────

    // Render a shadow call-stack as a backtrace string.
    static std::string format_call_stack(const std::vector<ShadowEntry>& call_stack);

    // Produce user-visible text for an exception (same at REPL and test harness).
    static std::string format_error(const std::exception& exc);

    // Return whitespace to auto-indent the next continuation line (3 spaces per
    // unclosed paren depth).
    static std::string compute_indent(const std::vector<std::string>& lines);

    // Parse a session log into a list of LogEntry structs.
    static std::vector<LogEntry> parse_log(const std::string& text);

    // Print the short welcome banner.
    static void print_welcome_banner();

private:
    // Class-level readline state (shared across all Listener instances in a process).
    static bool s_rl_initialized;
    static int  s_history_max;

    InterpreterBase* _interp;
    std::string      _testdir;
    std::string      _compliancedir;
    std::ofstream*   _logStream;    // nullptr when not logging
    std::string      _language;
    std::string      _version;
    std::string      _author;
    std::string      _project;

    std::unordered_map<std::string, std::function<void(std::vector<std::string>&)>> _commands;
    std::unordered_map<std::string, std::string> _help;

    void _init_readline();
    bool _use_color() const;
    void _banner();
    void _writeLn(const std::string& value = "",
                  std::ostream* file = nullptr,
                  bool flush = false);
    void _writeResult(const std::string& text);
    void _writeErrorMsg(const std::string& errMsg);
    std::string _prompt(const std::string& prompt  = "",
                        const std::string& prefill = "");
    void _runListenerCommand(const std::string& source);
    void _runTestFiles(const std::vector<std::string>& filenames);
    void _runComplianceFiles(const std::vector<std::string>& filenames,
                             const std::string& compliancedir);

    // Command handlers -- each corresponds to a ]command.
    void _cmd_help    (std::vector<std::string>& args);
    void _cmd_quit    (std::vector<std::string>& args);
    void _cmd_exit    (std::vector<std::string>& args);
    void _cmd_reboot  (std::vector<std::string>& args);
    void _cmd_readsrc (std::vector<std::string>& args);
    void _cmd_load    (std::vector<std::string>& args);
    void _cmd_readlog (std::vector<std::string>& args);
    void _cmd_log     (std::vector<std::string>& args);
    void _cmd_close   (std::vector<std::string>& args);
    void _cmd_resume  (std::vector<std::string>& args);
    void _cmd_test    (std::vector<std::string>& args);
    void _cmd_cd      (std::vector<std::string>& args);
    void _cmd_pwd     (std::vector<std::string>& args);
    void _cmd_lhistory(std::vector<std::string>& args);
    void _cmd_debug   (std::vector<std::string>& args);
    void _cmd_profile    (std::vector<std::string>& args);
    void _cmd_compliance (std::vector<std::string>& args);
};
