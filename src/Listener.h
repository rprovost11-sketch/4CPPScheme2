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
struct Environment; // AST.h forward-declares Environment as struct

// ── InterpreterBase ───────────────────────────────────────────────────────────
// Abstract interface that the Listener expects its interpreter to provide.
// Port of Listener.py InterpreterBase.

struct CPPSCHEME2_API InterpreterBase
   {
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
   virtual Context* get_ctx()
      {
      return nullptr;
      }
   virtual Environment* get_env()
      {
      return nullptr;
      }
   };

// ── ListenerCommandError ──────────────────────────────────────────────────────
// Raised by listener-command bodies to signal a user-level error.
// Port of Listener.py ListenerCommandError.
struct CPPSCHEME2_API ListenerCommandError : std::exception
   {
   explicit ListenerCommandError(const std::string& msg) : _msg(msg) {}
   const char* what() const noexcept override
      {
      return _msg.c_str();
      }

 private:
   std::string _msg;
   };

// ── TestResult ────────────────────────────────────────────────────────────────
// Return container for sessionLog_test: pass/fail counts.
// Port of Listener.py TestResult.
struct CPPSCHEME2_API TestResult
   {
   int n_pass;
   int n_fail;
   TestResult(int p, int f) : n_pass(p), n_fail(f) {}
   };

// ── Listener ──────────────────────────────────────────────────────────────────
// Interactive REPL with session logging and log-based testing.
// Port of Listener.py Listener.

class CPPSCHEME2_API Listener
   {
 public:
   // Each parsed log entry: expression, output, return value, error message.
   struct LogEntry
      {
      std::string expr;
      std::string output;
      std::string retval;
      std::string error;
      bool fold_case = false;
      };

   Listener(InterpreterBase* interp,
            const std::string& testdir = "feature-tests",
            const std::string& language = "cppscheme2",
            const std::string& version = "0.1",
            const std::string& author = "cppscheme2 authors",
            const std::string& project = "https://example/cppscheme2",
            const std::string& compliancedir = "",
            const std::string& regressiondir = "",
            const std::string& runsdir = "");
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
   static void print_welcome_banner(bool use_color);

 private:
   // Class-level readline state (shared across all Listener instances in a process).
   static bool s_rl_initialized;
   static int s_history_max;

   InterpreterBase* _interp;
   std::string _testdir;
   std::string _compliancedir;
   std::string _regressiondir;
   std::string _runsdir;
   std::ofstream* _logStream; // nullptr when not logging
   std::string _language;
   std::string _version;
   std::string _author;
   std::string _project;
   // True while a test run is redirecting std::cout to a run-report file.
   // Suppresses ANSI color so run files are clean text (cout's rdbuf swap is
   // invisible to isatty(); pyscheme gets this free via sys.stdout.isatty()).
   bool _output_to_file = false;
   // When true, ANSI color escape codes are emitted even when stdout is not a
   // TTY -- e.g. when the REPL is driven through a pipe by a GUI front-end
   // (cherry) that renders the codes itself.  Toggled with ]toggle-tty-color;
   // queried with ]tty-color.  Still suppressed while _output_to_file is set.
   bool _emit_color_codes = false;

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
   std::string _prompt(const std::string& prompt = "",
                       const std::string& prefill = "");
   void _runListenerCommand(const std::string& source);

   // Default value bound to %MAX_TCO_ITER_COUNT% before each test file (after
   // the per-file reboot); ]compliance -I:<count> overrides it.  Compliance
   // test 3.05 reads it to size its proper-tail-recursion soak loops.  Kept
   // modest so routine runs stay fast.
   static constexpr long long _TCO_ITER_DEFAULT = 100000;
   // Parse the value of an -I: switch: positive integer with optional metric
   // suffix (k/K = 1e3, m/M = 1e6).  Throws ListenerCommandError if malformed.
   static long long _parse_iter_count(const std::string& value);

   // Single test runner for all suites (feature / compliance / regression),
   // mirroring pyscheme's _runTestFiles.  `suite` is "feature" | "compliance"
   // | "regression"; it becomes part of the run-report filename:
   // yyyy-mm-dd-hhmmss-<suite>-CPPScheme2.run.  `tco_iters` is bound to
   // %MAX_TCO_ITER_COUNT% after each file's reboot.
   void _runTestFiles(const std::vector<std::string>& filenames, const std::string& testDir,
                      const std::string& suite, long long tco_iters = _TCO_ITER_DEFAULT);

   // Command handlers -- each corresponds to a ]command.
   void _cmd_help(std::vector<std::string>& args);
   void _cmd_quit(std::vector<std::string>& args);
   void _cmd_exit(std::vector<std::string>& args);
   void _cmd_reboot(std::vector<std::string>& args);
   void _cmd_readsrc(std::vector<std::string>& args);
   void _cmd_load(std::vector<std::string>& args);
   void _cmd_readlog(std::vector<std::string>& args);
   void _cmd_log(std::vector<std::string>& args);
   void _cmd_close(std::vector<std::string>& args);
   void _cmd_resume(std::vector<std::string>& args);
   void _cmd_feature(std::vector<std::string>& args);
   void _cmd_cd(std::vector<std::string>& args);
   void _cmd_pwd(std::vector<std::string>& args);
   void _cmd_toggle_tty_color(std::vector<std::string>& args);
   void _cmd_tty_color(std::vector<std::string>& args);
   void _print_tty_color_state();
   void _cmd_lhistory(std::vector<std::string>& args);
   void _cmd_debug(std::vector<std::string>& args);
   void _cmd_profile(std::vector<std::string>& args);
   void _cmd_compliance(std::vector<std::string>& args);
   void _cmd_regression(std::vector<std::string>& args);
   void _cmd_gc_stress(std::vector<std::string>& args);
   };
