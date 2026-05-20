// main.cpp — port of pyscheme/__main__.py + Interpreter.py + Listener.py
#include "gc.h"
#include "environment.h"
#include "symbol.h"
#include "value.h"
#include "parser.h"
#include "expander.h"
#include "analyzer.h"
#include "evaluator.h"
#include "primitives.h"
#include "exceptions.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <optional>
#include <map>
#include <chrono>
#include <iomanip>
#include <cstdlib>
#include <csignal>
#include <stdexcept>

#ifdef _WIN32
#  include <io.h>
#  include <direct.h>
#  define isatty _isatty
#  define fileno _fileno
#  define chdir  _chdir
#else
#  include <unistd.h>
#endif

static constexpr const char* CEKSCHEME_VERSION = "0.4.3";
static constexpr const char* CEKSCHEME_AUTHOR  = "Ron Provost/Longo";
static constexpr const char* CEKSCHEME_PROJECT = "https://github.com/rprovost11/cekscheme";
static constexpr const char* CEKSCHEME_TESTDIR = "testing";


// ── paren_state ───────────────────────────────────────────────────────────────

struct ParenState
   {
   int              depth;
   bool             in_string;
   std::vector<char> stack;
   };

static ParenState paren_state(const std::string& text)
   {
   std::vector<char> stack;
   bool in_string = false;
   bool escape    = false;
   size_t i = 0;
   size_t n = text.size();
   while (i < n)
      {
      char ch = text[i];
      if (escape)
         {
         escape = false;
         }
      else if (in_string)
         {
         if (ch == '\\') escape = true;
         else if (ch == '"') in_string = false;
         }
      else
         {
         if (ch == '"')
            in_string = true;
         else if (ch == ';')
            { while (i < n && text[i] != '\n') i++; continue; }
         else if (ch == '(' || ch == '[')
            stack.push_back(ch);
         else if (ch == ')' || ch == ']')
            { if (!stack.empty()) stack.pop_back(); }
         }
      i++;
      }
   return { (int)stack.size(), in_string, std::move(stack) };
   }


// ── retrieve_file_list ────────────────────────────────────────────────────────

static std::vector<std::string> retrieve_file_list(const std::string& dir)
   {
   std::vector<std::string> result;
   try
      {
      for (auto& e : std::filesystem::directory_iterator(dir))
         if (e.is_regular_file() && e.path().extension() == ".log")
            result.push_back(e.path().string());
      }
   catch (...) {}
   std::sort(result.begin(), result.end());
   return result;
   }


// ── columnize ─────────────────────────────────────────────────────────────────

static void columnize(const std::vector<std::string>& lst, int width,
                      const std::string& item_color = "")
   {
   static const std::string RESET = "\033[0m";
   int n = (int)lst.size();
   if (n == 0) return;
   if (n == 1)
      {
      if (!item_color.empty()) std::cout << item_color << lst[0] << RESET << "\n";
      else std::cout << lst[0] << "\n";
      return;
      }

   // Find smallest nrows such that all columns fit in width.
   int best_nrows = n;
   std::vector<int> best_cw;

   for (int nrows = 1; nrows < n; nrows++)
      {
      int ncols = (n + nrows - 1) / nrows;
      std::vector<int> cw(ncols, 0);
      int total = -2;
      bool fits = true;
      for (int col = 0; col < ncols; col++)
         {
         for (int row = 0; row < nrows; row++)
            {
            int idx = row + nrows * col;
            if (idx >= n) break;
            cw[col] = std::max(cw[col], (int)lst[idx].size());
            }
         total += cw[col] + 2;
         if (total > width) { fits = false; break; }
         }
      if (fits) { best_nrows = nrows; best_cw = cw; break; }
      }

   if (best_cw.empty())
      {
      for (auto& s : lst) std::cout << "  " << s << "\n";
      return;
      }

   int ncols = (int)best_cw.size();
   for (int row = 0; row < best_nrows; row++)
      {
      std::string line;
      for (int col = 0; col < ncols; col++)
         {
         int idx = row + best_nrows * col;
         if (idx >= n) break;
         const std::string& s = lst[idx];
         if (col > 0) line += "  ";
         if (!item_color.empty())
            line += item_color + s + RESET + std::string(best_cw[col] - (int)s.size(), ' ');
         else
            line += s + std::string(best_cw[col] - (int)s.size(), ' ');
         }
      // trim trailing spaces
      size_t end = line.find_last_not_of(' ');
      if (end != std::string::npos) line = line.substr(0, end + 1);
      std::cout << line << "\n";
      }
   }


// ── is_tty / ansi helpers ─────────────────────────────────────────────────────

static bool is_tty() { return isatty(fileno(stdout)) != 0; }

static std::string timestamp_iso()
   {
   auto now = std::chrono::system_clock::now();
   auto t   = std::chrono::system_clock::to_time_t(now);
   std::ostringstream ss;
   ss << std::put_time(std::localtime(&t), "%Y-%m-%dT%H:%M:%S");
   return ss.str();
   }


// ── OutputCapture ─────────────────────────────────────────────────────────────
// RAII wrapper: redirects primitive output into a string buffer.

struct OutputCapture
   {
   std::ostringstream buf;
   OutputCapture()  { set_primitive_output(&buf); }
   ~OutputCapture() { set_primitive_output(nullptr); }
   std::string str() const { return buf.str(); }
   };


// ── format_error ─────────────────────────────────────────────────────────────

static std::string format_error(const std::exception& exc)
   {
   if (auto* e = dynamic_cast<const SchemeRaisedException*>(&exc))
      return "SchemeRaised: " + value_to_string(e->raised, false);

   if (dynamic_cast<const SchemeArityError*>(&exc))
      {
      std::string msg = exc.what();
      const std::string pfx = "arity error: ";
      if (msg.size() >= pfx.size() && msg.substr(0, pfx.size()) == pfx)
         msg = msg.substr(pfx.size());
      return "SchemeArityError: " + msg;
      }

   if (dynamic_cast<const SchemeTypeError*>(&exc))
      return "SchemeTypeError: " + std::string(exc.what());

   if (dynamic_cast<const SchemeError*>(&exc))
      return exc.what();

   std::string msg = exc.what();
   return msg.empty() ? "internal error" : "internal error: " + msg;
   }


// ── Interpreter ───────────────────────────────────────────────────────────────

class Interpreter
   {
public:
   Interpreter()
      {
      gc_env_root_push(&env_);
      reboot();
      }

   ~Interpreter()
      {
      gc_env_root_pop(&env_);
      }

   void reboot(bool load_rc = true)
      {
      env_ = gc_alloc_environment(nullptr);
      install_primitives(env_);
      static_env_.clear();
      seed_static_env(static_env_);
      ctx_ = CekCtx{};

      if (load_rc)
         {
         std::string rc;
         const char* home = getenv("USERPROFILE");
         if (!home) home = getenv("HOME");
         if (home) rc = std::string(home) + "/.cekschemerc";
         if (!rc.empty() && std::filesystem::exists(rc))
            {
            try { evalFile(rc); }
            catch (const std::exception& e)
               {
               std::cerr << "cekscheme: error loading ~/.cekschemerc: "
                         << e.what() << "\n";
               }
            }
         }
      }

   std::optional<Value> rawEval(std::string_view source,
                                std::string_view filename = "")
      {
      (void)filename;
      ctx_.shadow_stack.clear();
      Parser p(source);
      std::optional<Value> last;
      while (!p.at_end())
         {
         auto opt = p.next();
         if (!opt) break;
         Value form     = *opt;
         Value expanded = expand(form, env_);
         analyze(expanded, &static_env_);
         extend_static_env_with_define(static_env_, expanded);
         last = cek_eval(expanded, env_, &ctx_);
         }
      return last;
      }

   std::string eval(std::string_view source)
      {
      auto raw = rawEval(source);
      if (!raw) return "";
      return value_to_string(*raw, false);
      }

   void evalFile(std::string_view path)
      {
      std::filesystem::path abs = std::filesystem::absolute(path);
      std::ifstream f(abs);
      if (!f)
         throw SchemeFileError("cannot open: " + std::string(path));
      std::string src((std::istreambuf_iterator<char>(f)), {});
      rawEval(src, abs.string());
      }

   Environment* env()        { return env_; }
   CekCtx&      ctx()        { return ctx_; }
   StaticEnv&   static_env() { return static_env_; }

private:
   Environment* env_        = nullptr;
   StaticEnv    static_env_;
   CekCtx       ctx_;
   };


// ── Listener ──────────────────────────────────────────────────────────────────

class ListenerCommandError : public std::runtime_error
   {
public:
   explicit ListenerCommandError(const std::string& msg)
      : std::runtime_error(msg) {}
   };

struct QuitListener {};

struct LogEntry
   {
   std::string expr;
   std::string output;
   std::string retval;
   std::string error;
   };

struct TestResult { int n_pass; int n_fail; };


class Listener
   {
public:
   Listener(Interpreter& interp,
            std::string  testdir,
            std::string  language,
            std::string  version,
            std::string  author,
            std::string  project)
      : interp_(interp)
      , testdir_(std::filesystem::absolute(testdir).string())
      , language_(std::move(language))
      , version_(std::move(version))
      , author_(std::move(author))
      , project_(std::move(project))
      , log_file_(nullptr)
      {
      build_commands();
      banner();
      }

   ~Listener()
      {
      if (log_file_) { fclose(log_file_); log_file_ = nullptr; }
      }

   void readEvalPrintLoop()
      {
      std::vector<std::string> lines;

      while (true)
         {
         std::string line;
         std::string prompt = lines.empty() ? ">>> " : "... ";
         if (!lines.empty())
            {
            std::string combined;
            for (auto& l : lines) { if (!combined.empty()) combined += '\n'; combined += l; }
            std::string indent = compute_indent(combined);
            std::cout << prompt;
            std::cout.flush();
            if (!std::getline(std::cin, line))
               { std::cout << "\n"; break; }
            // prepend auto-indent for continuation lines
            if (!indent.empty() && !line.empty())
               line = indent + line;
            }
         else
            {
            std::cout << prompt;
            std::cout.flush();
            if (!std::getline(std::cin, line))
               { std::cout << "\n"; break; }
            }

         // Super-bracket: trailing ']' (unless the line starts a listener command).
         if (!line.empty() && line.back() == ']' &&
             !(line.size() > 1 && line[0] == ']'))
            {
            std::string tentative = line.substr(0, line.size() - 1);
            std::string combined;
            for (auto& l : lines) { if (!combined.empty()) combined += '\n'; combined += l; }
            if (!tentative.empty()) { if (!combined.empty()) combined += '\n'; combined += tentative; }
            ParenState ps = paren_state(combined);
            bool innermost_bracket = !ps.stack.empty() && ps.stack.back() == '[';
            if (ps.depth > 0 && !ps.in_string && !innermost_bracket)
               line = tentative + std::string(ps.depth, ')');
            else if (line == "]" && ps.depth == 0 && !ps.in_string)
               continue;
            }

         // Mirror to log (non-command lines only).
         if (log_file_ && !line.empty() && line[0] != ']')
            {
            std::string log_prompt = lines.empty() ? ">>> " : "... ";
            fprintf(log_file_, "%s%s\n", log_prompt.c_str(), line.c_str());
            fflush(log_file_);
            }

         if (line.empty())
            {
            if (!lines.empty())
               { /* fall through to submit */ }
            else
               continue;
            }
         else
            {
            lines.push_back(line);
            std::string combined;
            for (auto& l : lines) { if (!combined.empty()) combined += '\n'; combined += l; }
            ParenState ps = paren_state(combined);
            if (ps.depth > 0 || ps.in_string) continue;
            }

         // Assemble and submit.
         std::string expr;
         for (auto& l : lines) { if (!expr.empty()) expr += '\n'; expr += l; }
         lines.clear();

         // Trim.
         size_t start = expr.find_first_not_of(" \t\r\n");
         if (start == std::string::npos) continue;
         expr = expr.substr(start);
         size_t end = expr.find_last_not_of(" \t\r\n");
         if (end != std::string::npos) expr = expr.substr(0, end + 1);
         if (expr.empty()) continue;

         try
            {
            if (expr[0] == ']')
               run_listener_command(expr);
            else
               {
               std::string result = interp_.eval(expr);
               write_result(result);
               }
            }
         catch (const QuitListener&) { throw; }
         catch (const ListenerCommandError& e)
            { write_error_msg(e.what()); }
         catch (const std::exception& e)
            { write_error_msg(format_error(e)); }
         std::cout << "\n";
         }
      }

private:
   // ── static helpers ────────────────────────────────────────────────────────

   static std::string compute_indent(const std::string& text)
      {
      int depth = 0;
      bool in_string = false;
      bool escape    = false;
      for (char ch : text)
         {
         if (escape) { escape = false; continue; }
         if (in_string)
            {
            if (ch == '\\') escape = true;
            else if (ch == '"') in_string = false;
            continue;
            }
         if (ch == '"') { in_string = true; continue; }
         if (ch == ';') { while (ch != '\n') break; continue; }
         if (ch == '(' || ch == '[') depth++;
         else if ((ch == ')' || ch == ']') && depth > 0) depth--;
         }
      return std::string(depth * 3, ' ');
      }

   static std::vector<LogEntry> parse_log(const std::string& text)
      {
      std::vector<LogEntry> entries;
      std::vector<std::string_view> lines;
      {
      size_t pos = 0;
      while (pos <= text.size())
         {
         size_t nl = text.find('\n', pos);
         if (nl == std::string::npos) nl = text.size();
         lines.push_back(std::string_view(text).substr(pos, nl - pos + (nl < text.size() ? 1 : 0)));
         if (nl == text.size()) break;
         pos = nl + 1;
         }
      }

      size_t idx = 0;
      size_t n   = lines.size();

      auto strip_newline = [](std::string_view sv) -> std::string {
         std::string s(sv);
         while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
         return s;
         };

      while (idx < n)
         {
         // Advance to next ">>> " line.
         while (idx < n && lines[idx].substr(0, 4) != ">>> ") idx++;
         if (idx >= n) break;

         std::string expr   = strip_newline(lines[idx].substr(4));
         std::string output;
         std::string retval;
         std::string error;
         idx++;

         // Continuation "... " lines.
         while (idx < n && lines[idx].substr(0, 4) == "... ")
            {
            expr += '\n';
            expr += strip_newline(lines[idx].substr(4));
            idx++;
            }
         // Bare "..." line (obsolete format).
         if (idx < n && strip_newline(lines[idx]) == "..." &&
             lines[idx].substr(0, 4) != "... ")
            idx++;

         // Output lines (until ==> or %%% or >>>/... marker).
         while (idx < n)
            {
            std::string_view l = lines[idx];
            if (l.substr(0, 4) == "==> " || strip_newline(l) == "==>" ||
                l.substr(0, 4) == "... " || l.substr(0, 4) == ">>> " ||
                l.substr(0, 4) == "%%% ")
               break;
            output += strip_newline(l); output += '\n';
            idx++;
            }

         // Return value "==> " block.
         if (idx < n && (lines[idx].substr(0, 4) == "==> " ||
                         strip_newline(lines[idx]) == "==>"))
            {
            std::string_view l = lines[idx];
            if (l.size() > 4) retval = strip_newline(l.substr(4));
            idx++;
            while (idx < n)
               {
               std::string_view l2 = lines[idx];
               if (l2.substr(0, 4) == "==> " || strip_newline(l2) == "==>" ||
                   l2.substr(0, 4) == "... " || l2.substr(0, 4) == ">>> " ||
                   l2.substr(0, 4) == "%%% ")
                  break;
               if (!l2.empty() && l2[0] == ';')
                  expr += '\n'; // append comment to expr (matches Python)
               else
                  { retval += '\n'; retval += strip_newline(l2); }
               idx++;
               }
            }

         // Error "%%% " block.
         if (idx < n && lines[idx].substr(0, 4) == "%%% ")
            {
            error = strip_newline(lines[idx].substr(4));
            idx++;
            while (idx < n && lines[idx].substr(0, 4) == "%%% ")
               {
               error += '\n';
               error += strip_newline(lines[idx].substr(4));
               idx++;
               }
            }

         // Trim trailing whitespace.
         while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' '))
            output.pop_back();
         while (!retval.empty() && (retval.back() == '\n' || retval.back() == '\r' || retval.back() == ' '))
            retval.pop_back();
         while (!error.empty() && (error.back() == '\n' || error.back() == '\r' || error.back() == ' '))
            error.pop_back();

         if (!expr.empty())
            entries.push_back({ std::move(expr), std::move(output),
                                 std::move(retval), std::move(error) });
         }
      return entries;
      }

   static void print_welcome_banner()
      {
      bool color = is_tty();
      const std::string BOLD_GREEN = color ? "\033[1;92m" : "";
      const std::string CYAN       = color ? "\033[96m"   : "";
      const std::string RESET      = color ? "\033[0m"    : "";
      std::cout << "Enter any expression to have it evaluated by the interpreter.\n";
      std::cout << "Evaluate '" << CYAN << "(help)" << RESET << "' for online help.\n";
      std::cout << "Type  '" << CYAN << "]help" << RESET << "' to list Listener commands.\n";
      std::cout << BOLD_GREEN << "Welcome!" << RESET << "\n";
      }

   // ── I/O helpers ───────────────────────────────────────────────────────────

   void writeln(const std::string& text = "", bool flush = false)
      {
      std::cout << text << "\n";
      if (flush) std::cout.flush();
      if (log_file_) { fprintf(log_file_, "%s\n", text.c_str()); fflush(log_file_); }
      }

   void write_result(const std::string& text)
      {
      bool color = is_tty();
      const std::string GREEN = color ? "\033[92m"   : "";
      const std::string BOLD  = color ? "\033[1;97m" : "";
      const std::string RESET = color ? "\033[0m"    : "";

      std::string t = text;
      // Find lines by splitting on '\n'.
      std::vector<std::string> lines;
      size_t pos = 0;
      while (pos <= t.size())
         {
         size_t nl = t.find('\n', pos);
         if (nl == std::string::npos) { lines.push_back(t.substr(pos)); break; }
         lines.push_back(t.substr(pos, nl - pos));
         pos = nl + 1;
         }
      if (lines.empty()) lines.push_back("");

      for (auto& line : lines)
         {
         std::string plain = "==> " + line;
         if (color)
            std::cout << GREEN << "==>" << RESET << " " << BOLD << line << RESET << "\n";
         else
            std::cout << plain << "\n";
         std::cout.flush();
         if (log_file_) { fprintf(log_file_, "%s\n", plain.c_str()); fflush(log_file_); }
         }
      }

   void write_error_msg(const std::string& err_msg)
      {
      bool color = is_tty();
      const std::string RED   = color ? "\033[91m" : "";
      const std::string RESET = color ? "\033[0m"  : "";

      std::vector<std::string> lines;
      size_t pos = 0;
      while (pos <= err_msg.size())
         {
         size_t nl = err_msg.find('\n', pos);
         if (nl == std::string::npos) { lines.push_back(err_msg.substr(pos)); break; }
         lines.push_back(err_msg.substr(pos, nl - pos));
         pos = nl + 1;
         }
      if (lines.empty()) lines.push_back(err_msg);

      for (auto& line : lines)
         {
         std::string plain = "%%% " + line;
         if (color) std::cout << RED << plain << RESET << "\n";
         else       std::cout << plain << "\n";
         std::cout.flush();
         if (log_file_) { fprintf(log_file_, "%s\n", plain.c_str()); fflush(log_file_); }
         }
      }

   void banner()
      {
      bool color = is_tty();
      const std::string BOLD_WHITE = color ? "\033[1;97m" : "";
      const std::string DIM        = color ? "\033[2m"    : "";
      const std::string RESET      = color ? "\033[0m"    : "";
      std::cout << BOLD_WHITE << language_ << " " << version_
                << " by " << author_ << RESET << "\n";
      std::cout << DIM << "Project home " << project_ << RESET << "\n";
      std::cout << "\n";
      std::cout << DIM << "- Interpreter Initialized" << RESET << "\n";
      std::cout << DIM << "- Listener Initialized"    << RESET << "\n";
      std::cout << "\n";
      print_welcome_banner();
      std::cout << "\n";
      std::cout.flush();
      }

   // ── session log restore / test ────────────────────────────────────────────

   void session_log_restore(const std::string& filename, int verbosity = 0)
      {
      std::ifstream f(filename);
      if (!f) throw ListenerCommandError("File not found: " + filename);
      std::string text((std::istreambuf_iterator<char>(f)), {});
      f.close();

      auto entries = parse_log(text);
      for (auto& e : entries)
         {
         if (verbosity > 0)
            {
            std::vector<std::string> exp_lines;
            size_t pos = 0;
            while (pos <= e.expr.size())
               {
               size_t nl = e.expr.find('\n', pos);
               if (nl == std::string::npos) { exp_lines.push_back(e.expr.substr(pos)); break; }
               exp_lines.push_back(e.expr.substr(pos, nl - pos));
               pos = nl + 1;
               }
            for (size_t j = 0; j < exp_lines.size(); j++)
               std::cout << (j == 0 ? "\n>>> " : "... ") << exp_lines[j] << "\n";
            }
         try { interp_.eval(e.expr); }
         catch (...) {}
         }
      }

   TestResult session_log_test(const std::string& filename, int verbosity = 3)
      {
      std::ifstream f(filename);
      if (!f) throw ListenerCommandError("File not found: " + filename);
      std::string text((std::istreambuf_iterator<char>(f)), {});
      f.close();

      bool color = is_tty();
      const std::string BOLD  = color ? "\033[1;97m" : "";
      const std::string DIM   = color ? "\033[2m"    : "";
      const std::string GREEN = color ? "\033[92m"   : "";
      const std::string RED   = color ? "\033[91m"   : "";
      const std::string RESET = color ? "\033[0m"    : "";

      auto entries = parse_log(text);
      int n_pass   = 0;
      int n_fail   = 0;

      std::cout << "\n";
      std::cout << BOLD << "Test file:" << RESET << " " << filename << "\n";
      std::cout << BOLD << std::string(11 + filename.size(), '-') << RESET << "\n";

      for (int k = 0; k < (int)entries.size(); k++)
         {
         auto& entry = entries[k];
         int i = k + 1;

         std::string actual_retval;
         std::string actual_error;

         OutputCapture cap;
         try
            {
            actual_retval = interp_.eval(entry.expr);
            }
         catch (const std::exception& ex)
            {
            actual_error = format_error(ex);
            }

         std::string actual_output = cap.str();
         // trim trailing whitespace
         while (!actual_output.empty() && (actual_output.back() == '\n' ||
                actual_output.back() == '\r' || actual_output.back() == ' '))
            actual_output.pop_back();
         std::string expected_output = entry.output;

         bool retval_ok = (actual_retval == entry.retval);
         bool error_ok  = (actual_error  == entry.error);
         bool output_ok = (actual_output == expected_output);

         // Label: first line of expression, truncated to 56 chars.
         std::string label = entry.expr;
         size_t nl = label.find('\n');
         if (nl != std::string::npos) label = label.substr(0, nl);
         if (label.size() > 56) label = label.substr(0, 53) + "...";

         if (retval_ok && error_ok && output_ok)
            {
            n_pass++;
            if (verbosity >= 3)
               std::cout << DIM << "  " << std::setw(3) << i
                         << ". PASS  " << label << RESET << "\n";
            }
         else
            {
            n_fail++;
            std::cout << RED << "  " << std::setw(3) << i
                      << ". FAIL  " << label << RESET << "\n";
            if (!retval_ok)
               {
               std::cout << "         expected return: [" << entry.retval  << "]\n";
               std::cout << "         actual return:   [" << actual_retval << "]\n";
               }
            if (!output_ok)
               {
               std::cout << "         expected output: [" << expected_output << "]\n";
               std::cout << "         actual output:   [" << actual_output   << "]\n";
               }
            if (!error_ok)
               {
               std::cout << "         expected error:  [" << entry.error  << "]\n";
               std::cout << "         actual error:    [" << actual_error << "]\n";
               }
            }
         }

      std::cout << "\n";
      if (n_fail == 0)
         std::cout << GREEN << (n_pass + n_fail) << " TESTS PASSED" << RESET << "\n";
      else
         std::cout << RED << n_fail << " of " << (n_pass + n_fail)
                   << " FAILED" << RESET << "\n";

      return { n_pass, n_fail };
      }

   // ── command dispatch ──────────────────────────────────────────────────────

   using CmdFn = std::function<void(const std::vector<std::string>&)>;

   void build_commands()
      {
      cmds_["help"]     = [this](auto& a){ cmd_help(a);     };
      cmds_["quit"]     = [this](auto& a){ cmd_quit(a);     };
      cmds_["exit"]     = [this](auto& a){ cmd_quit(a);     };
      cmds_["reboot"]   = [this](auto& a){ cmd_reboot(a);   };
      cmds_["readsrc"]  = [this](auto& a){ cmd_readsrc(a);  };
      cmds_["load"]     = [this](auto& a){ cmd_readsrc(a);  };
      cmds_["readlog"]  = [this](auto& a){ cmd_readlog(a);  };
      cmds_["log"]      = [this](auto& a){ cmd_log(a);      };
      cmds_["close"]    = [this](auto& a){ cmd_close(a);    };
      cmds_["resume"]   = [this](auto& a){ cmd_resume(a);   };
      cmds_["test"]     = [this](auto& a){ cmd_test(a);     };
      cmds_["cd"]       = [this](auto& a){ cmd_cd(a);       };
      cmds_["pwd"]      = [this](auto& a){ cmd_pwd(a);      };
      cmds_["lhistory"] = [this](auto& a){ cmd_lhistory(a); };
      cmds_["debug"]    = [this](auto& a){ cmd_debug(a);    };
      }

   void run_listener_command(const std::string& source)
      {
      std::string body = source.substr(1);
      std::vector<std::string> parts;
      size_t pos = 0;
      while (pos < body.size())
         {
         while (pos < body.size() && body[pos] == ' ') pos++;
         if (pos >= body.size()) break;
         size_t start = pos;
         while (pos < body.size() && body[pos] != ' ') pos++;
         parts.push_back(body.substr(start, pos - start));
         }
      if (parts.empty())
         throw ListenerCommandError("expected a command after ']'");

      std::string cmd  = parts[0];
      std::vector<std::string> args(parts.begin() + 1, parts.end());

      auto it = cmds_.find(cmd);
      if (it == cmds_.end())
         throw ListenerCommandError("Unknown listener command: " + cmd);
      it->second(args);
      }

   // ── commands ──────────────────────────────────────────────────────────────

   void cmd_help(const std::vector<std::string>& args)
      {
      bool color = is_tty();
      const std::string BOLD  = color ? "\033[1;97m" : "";
      const std::string CYAN  = color ? "\033[96m"   : "";
      const std::string RESET = color ? "\033[0m"    : "";

      if (!args.empty())
         {
         auto it = help_text_.find(args[0]);
         if (it == help_text_.end())
            throw ListenerCommandError("No help on \"" + args[0] + "\".");
         std::cout << it->second << "\n";
         return;
         }

      std::vector<std::string> names;
      for (auto& [k, _] : cmds_) names.push_back(k);
      std::sort(names.begin(), names.end());

      std::string header = "Listener Commands";
      std::cout << "\n" << BOLD << header << RESET << "\n";
      std::cout << BOLD << std::string(header.size(), '=') << RESET << "\n";
      columnize(names, 69, CYAN);
      std::cout << "\n";
      std::cout << "Type ']help <command>' for detailed help on a command.\n";
      }

   void cmd_quit(const std::vector<std::string>& args)
      {
      if (!args.empty()) throw ListenerCommandError("Usage: ]quit");
      if (log_file_) cmd_close({});
      std::cout << "Bye.\n";
      throw QuitListener{};
      }

   void cmd_reboot(const std::vector<std::string>& args)
      {
      if (!args.empty()) throw ListenerCommandError("Usage: ]reboot");
      if (log_file_)
         throw ListenerCommandError(
            "Please close the log file before rebooting (]close).");
      bool color = is_tty();
      const std::string DIM   = color ? "\033[2m" : "";
      const std::string RESET = color ? "\033[0m" : "";
      std::cout << DIM << "- Initializing interpreter" << RESET << "\n";
      interp_.reboot();
      std::cout << "\n";
      print_welcome_banner();
      std::cout << "\n";
      }

   void cmd_readsrc(const std::vector<std::string>& args)
      {
      if (args.size() != 1)
         throw ListenerCommandError("Usage: ]readsrc <filename>");
      try { interp_.evalFile(args[0]); }
      catch (const SchemeFileError&)
         { throw ListenerCommandError("File not found: " + args[0]); }
      bool color = is_tty();
      const std::string GREEN = color ? "\033[92m" : "";
      const std::string RESET = color ? "\033[0m"  : "";
      std::cout << GREEN << "Source file read successfully:" << RESET
                << " " << args[0] << "\n";
      }

   void cmd_readlog(const std::vector<std::string>& args)
      {
      if (args.size() < 1 || args.size() > 2)
         throw ListenerCommandError("Usage: ]readlog <filename> [v|V]");
      int verbosity = 0;
      if (args.size() == 2)
         {
         std::string flag = args[1];
         if (flag == "v" || flag == "V") verbosity = 3;
         }
      session_log_restore(args[0], verbosity);
      bool color = is_tty();
      const std::string GREEN = color ? "\033[92m" : "";
      const std::string RESET = color ? "\033[0m"  : "";
      std::cout << GREEN << "Log file read successfully:" << RESET
                << " " << args[0] << "\n";
      }

   void cmd_log(const std::vector<std::string>& args)
      {
      if (args.size() != 1)
         throw ListenerCommandError("Usage: ]log <filename>");
      if (log_file_)
         throw ListenerCommandError(
            "Already logging.  Close the current log first (]close).");
      log_file_ = fopen(args[0].c_str(), "w");
      if (!log_file_)
         throw ListenerCommandError("Unable to open file for writing.");
      log_filename_ = args[0];
      std::string ts = ";;; Dribble started " + timestamp_iso();
      writeln(ts);
      writeln(";;; " + args[0]);
      writeln();
      }

   void cmd_close(const std::vector<std::string>& args)
      {
      if (!args.empty()) throw ListenerCommandError("Usage: ]close");
      if (!log_file_)
         throw ListenerCommandError("Not currently logging.");
      writeln();
      writeln(";;; Dribble stopped " + timestamp_iso());
      fclose(log_file_);
      log_file_    = nullptr;
      log_filename_ = "";
      }

   void cmd_resume(const std::vector<std::string>& args)
      {
      if (log_file_)
         throw ListenerCommandError(
            "A log file is already open.  Close it first (]close).");
      if (args.size() != 1)
         throw ListenerCommandError("Usage: ]resume <filename>");
      session_log_restore(args[0]);
      log_file_ = fopen(args[0].c_str(), "a");
      if (!log_file_)
         throw ListenerCommandError("Unable to reopen file for append.");
      log_filename_ = args[0];
      writeln();
      writeln(";;; Dribble resumed " + timestamp_iso());
      writeln();
      }

   void cmd_test(const std::vector<std::string>& args)
      {
      if (args.size() > 1)
         throw ListenerCommandError("Usage: ]test [<filename>]");
      if (log_file_)
         throw ListenerCommandError(
            "Please close the log before running tests (]close).");

      std::vector<std::string> filenames;
      if (!args.empty())
         {
         filenames.push_back(args[0]);
         }
      else
         {
         if (!std::filesystem::is_directory(testdir_))
            throw ListenerCommandError("No test directory: " + testdir_);
         filenames = retrieve_file_list(testdir_);
         if (filenames.empty())
            throw ListenerCommandError("No .log files in " + testdir_);
         }

      run_test_files(filenames);
      }

   void run_test_files(const std::vector<std::string>& filenames)
      {
      bool color = is_tty();
      const std::string BOLD  = color ? "\033[1;97m" : "";
      const std::string GREEN = color ? "\033[92m"   : "";
      const std::string RED   = color ? "\033[91m"   : "";
      const std::string RESET = color ? "\033[0m"    : "";

      // Prepare run-report file.
      FILE*       run_file     = nullptr;
      std::string run_filename;
      if (filenames.size() > 1)
         {
         std::string runs_dir = testdir_ + "/runs";
         try
            {
            std::filesystem::create_directories(runs_dir);
            auto now = std::chrono::system_clock::now();
            auto t   = std::chrono::system_clock::to_time_t(now);
            std::ostringstream ts;
            ts << std::put_time(std::localtime(&t), "%Y-%m-%d-%H%M%S");
            run_filename = runs_dir + "/test-" + ts.str() + ".run";
            run_file = fopen(run_filename.c_str(), "w");
            }
         catch (...) {}
         }

      int grand_pass = 0;
      int grand_fail = 0;
      struct FileResult { std::string name; int p; int f; };
      std::vector<FileResult> per_file;

      std::ofstream run_stream;
      for (auto& filename : filenames)
         {
         interp_.reboot(false);

         std::string base   = std::filesystem::path(filename).filename().string();
         std::string padded = base;
         while ((int)padded.size() < 40) padded += ' ';
         std::cout << padded << " ";
         std::cout.flush();

         // Redirect cout to run_file if we have one.
         std::streambuf* old_buf = nullptr;
         if (run_file)
            {
            // We can't easily redirect cout to FILE*; use a stringstream approach.
            run_stream.open(run_filename, std::ios::app);
            old_buf = std::cout.rdbuf(run_stream.rdbuf());
            }

         TestResult r = session_log_test(filename, 3);

         if (run_file && old_buf)
            {
            std::cout.rdbuf(old_buf);
            run_stream.close();
            }

         grand_pass += r.n_pass;
         grand_fail += r.n_fail;
         per_file.push_back({ filename, r.n_pass, r.n_fail });

         std::string status;
         if (r.n_fail == 0)
            status = GREEN + std::to_string(r.n_pass) + " passed" + RESET;
         else
            status = RED + std::to_string(r.n_fail) + " of "
                     + std::to_string(r.n_pass + r.n_fail) + " failed" + RESET;
         std::cout << status << "\n";
         std::cout.flush();
         }

      interp_.reboot(false);

      if (filenames.size() > 1)
         {
         std::cout << "\n";
         int total = grand_pass + grand_fail;
         if (grand_fail == 0)
            std::cout << GREEN << total << " TESTS PASSED across "
                      << filenames.size() << " files" << RESET << "\n";
         else
            std::cout << RED << grand_fail << " of " << total
                      << " FAILED across " << filenames.size() << " files" << RESET << "\n";

         if (run_file)
            {
            fprintf(run_file, "\n\nTest Report\n===========\n");
            for (auto& pf : per_file)
               {
               std::string short_name = std::filesystem::path(pf.name).filename().string();
               std::string msg;
               if (pf.f == 0) msg = std::to_string(pf.p) + " TESTS PASSED!";
               else msg = "(" + std::to_string(pf.f) + "/" + std::to_string(pf.p + pf.f) + ") Failed.";
               while ((int)short_name.size() < 40) short_name += ' ';
               fprintf(run_file, "%s %s\n", short_name.c_str(), msg.c_str());
               }
            fprintf(run_file, "\nTotal test files: %d.\nTotal test cases: %d.\n",
                    (int)filenames.size(), grand_pass + grand_fail);
            fclose(run_file);
            std::cout << "\nTest output: " << run_filename << "\n";
            }
         }
      }

   void cmd_cd(const std::vector<std::string>& args)
      {
      if (args.size() != 1)
         throw ListenerCommandError("Usage: ]cd <directory>");
      std::string target = args[0];
      // Expand ~
      if (!target.empty() && target[0] == '~')
         {
         const char* home = getenv("USERPROFILE");
         if (!home) home = getenv("HOME");
         if (home) target = std::string(home) + target.substr(1);
         }
      if (!std::filesystem::is_directory(target))
         throw ListenerCommandError("Not a directory: " + target);
      if (chdir(target.c_str()) != 0)
         throw ListenerCommandError("chdir failed: " + target);
      bool color = is_tty();
      const std::string DIM   = color ? "\033[2m" : "";
      const std::string RESET = color ? "\033[0m" : "";
      std::cout << DIM << std::filesystem::current_path().string() << RESET << "\n";
      }

   void cmd_pwd(const std::vector<std::string>& args)
      {
      if (!args.empty()) throw ListenerCommandError("Usage: ]pwd");
      std::cout << std::filesystem::current_path().string() << "\n";
      }

   void cmd_lhistory(const std::vector<std::string>& args)
      {
      if (args.size() > 1)
         throw ListenerCommandError("Usage: ]lhistory [<n>]");
      if (args.empty())
         { std::cout << "History not available (no readline).\n"; return; }
      try { int n = std::stoi(args[0]); (void)n; }
      catch (...) { throw ListenerCommandError("History size must be an integer."); }
      std::cout << "History not available (no readline).\n";
      }

   void cmd_debug(const std::vector<std::string>& args)
      {
      if (!args.empty()) throw ListenerCommandError("Usage: ]debug");
      std::cout << "]debug not yet implemented in cekscheme.\n";
      }

   // ── data members ──────────────────────────────────────────────────────────

   Interpreter& interp_;
   std::string  testdir_;
   std::string  language_;
   std::string  version_;
   std::string  author_;
   std::string  project_;
   FILE*        log_file_    = nullptr;
   std::string  log_filename_;
   std::map<std::string, CmdFn>         cmds_;
   std::map<std::string, std::string>   help_text_;
   };


// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
   {
#ifdef SIGBREAK
   signal(SIGBREAK, SIG_DFL);
#endif

   if (argc > 2)
      {
      std::cerr << "Usage: cekscheme [<directory> | <scheme-source-file>]\n";
      return 2;
      }

   gc_init();

   Interpreter interp;

   if (argc == 2)
      {
      std::string target = argv[1];
      if (std::filesystem::is_directory(target))
         {
         if (chdir(target.c_str()) != 0)
            {
            std::cerr << "cekscheme: cannot chdir to: " << target << "\n";
            gc_shutdown();
            return 1;
            }
         // fall through to REPL
         }
      else if (std::filesystem::is_regular_file(target))
         {
         try
            { interp.evalFile(target); }
         catch (const std::exception& e)
            {
            std::cerr << "cekscheme: " << e.what() << "\n";
            gc_shutdown();
            return 1;
            }
         gc_shutdown();
         return 0;
         }
      else
         {
         std::cerr << "cekscheme: no such file or directory: " << target << "\n";
         gc_shutdown();
         return 1;
         }
      }

   Listener listener(
      interp,
      CEKSCHEME_TESTDIR,
      "cekscheme",
      CEKSCHEME_VERSION,
      CEKSCHEME_AUTHOR,
      CEKSCHEME_PROJECT
   );

   try
      { listener.readEvalPrintLoop(); }
   catch (const QuitListener&) {}

   gc_shutdown();
   return 0;
   }
