#pragma once
// Listener.h -- Scheme REPL listener.
// Direct port of pyscheme/Listener.py.
#include "AST.h"
#include "scheme_export.h"
#include <exception>
#include <fstream>
#include <functional>
#include <iosfwd>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
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
            const std::string& language = "cppscheme2",
            const std::string& version = "0.1",
            const std::string& author = "cppscheme2 authors",
            const std::string& project = "https://example/cppscheme2",
            const std::string& scheme_tests_dir = "",
            const std::string& scheme_tests_source = "unset",
            bool show_banner = true);
   ~Listener();

   // Run the interactive REPL until EOF or ]quit/]exit.
   void readEvalPrintLoop();

   // Evaluate each -e/--evaluate expression as a full REPL transcript (echo the
   // input with '>>> '/'... ' prompts, run it, show '==> value' or '%%% error'),
   // then return a process exit status (1 if any expression raised, else 0).
   int eval_and_exit(const std::vector<std::string>& expressions);

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
   // The scheme-tests root (from -T/--scheme-tests, $SCHEME_TESTS_DIR, or the
   // ]scheme-tests command; never hardcoded) and the four suite directories
   // derived from it (hardcoded relative subpaths -- a config file is the
   // eventual home).  All empty when no root is set.
   std::string _scheme_tests_dir;
   std::string _scheme_tests_source;
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
   // When ]suites runs several suites, it opens ONE shared .run report and
   // parks the handle here; each _runTestFiles appends its section instead of
   // opening (and closing) its own file.  nullptr for individual commands.
   std::ofstream* _shared_run_file = nullptr;
   std::string _shared_run_filename;
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
   // Returns the grand pass/fail totals so ]suites can aggregate a combined
   // verdict across several suites.
   TestResult _runTestFiles(const std::vector<std::string>& filenames, const std::string& testDir,
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
   TestResult _cmd_feature(std::vector<std::string>& args);
   void _cmd_cd(std::vector<std::string>& args);
   void _cmd_pwd(std::vector<std::string>& args);
   void _cmd_toggle_tty_color(std::vector<std::string>& args);
   void _cmd_tty_color(std::vector<std::string>& args);
   void _print_tty_color_state();
   void _cmd_lhistory(std::vector<std::string>& args);
   void _cmd_debug(std::vector<std::string>& args);
   void _cmd_profile(std::vector<std::string>& args);
   TestResult _cmd_compliance(std::vector<std::string>& args);
   TestResult _cmd_regression(std::vector<std::string>& args);
   void _cmd_gc_stress(std::vector<std::string>& args);
   void _cmd_suites(std::vector<std::string>& args);

   // ── registry-driven ]suites (backlog #9) ──────────────────────────────────
   // Minimal S-expression node for the registry reader: an atom (symbol / string
   // / number, all kept as a plain string) or a list.  Structure is the only
   // distinction the registry needs.
   struct SForm { bool isList = false; std::string atom; std::vector<SForm> list; };
   // A parsed (suite ...) entry from scheme-tests/test-suites.scm.  `label` and
   // the applied variant are filled transiently when a run is dispatched.
   struct SuiteDef
      {
      std::string name, kind, ports = "both", path, cwd = ".", desc, passGrep, label;
      std::vector<std::string> alias, categories, libs, run;
      bool passExit0 = true;       // false => external pass is (grep passGrep)
      long long tcoSoak = -1;      // -1 => use _TCO_ITER_DEFAULT
      bool tcoCalibrate = false;   // (tco-soak calibrate) -> calibrate at runtime
      // variant name -> its override prop forms (applied over the base on demand)
      std::map<std::string, std::vector<SForm>> variants;
      };
   struct SuiteRunResult
      {
      std::string name;
      bool ok = false;
      int npass = 0, nfail = 0, nxpass = 0;
      std::string note;
      };
   static std::vector<SForm> _read_sexprs(const std::string& text);
   static void _parse_props(const std::vector<SForm>& props, SuiteDef& into);
   static void _parse_test_output(const std::string& out, int& npass, int& nfail, int& nxpass);
   static std::string _run_capture(const std::string& cmd, int& exitCode);
   static std::vector<std::string> _selector_matches(const std::string& sel,
                                                     const std::vector<SuiteDef>& suites);
   std::string _port_tag() const;
   std::string _registry_path() const;
   std::string _suite_abspath(const std::string& rel) const;
   std::string _self_exe_path() const;
   std::vector<SuiteDef> _load_suites();
   // (suite, variant-name) selections in registry order, deduped.
   std::vector<std::pair<SuiteDef, std::string>> _resolve_suite_tokens(
       const std::vector<std::string>& tokens, const std::vector<SuiteDef>& suites);
   void _print_suite_list(const std::vector<SuiteDef>& suites);
   SuiteRunResult _run_log_suite(const SuiteDef& s);
   SuiteRunResult _run_scheme_suite(const SuiteDef& s);
   SuiteRunResult _run_external_suite(const SuiteDef& s);
   void _cmd_scheme_tests(std::vector<std::string>& args);
   // scheme-tests root resolution (no path hardcoded; subdirs derived from root).
   void _set_scheme_tests_dir(const std::string& path, const std::string& source);
   void _require_scheme_tests();
   static std::string _no_scheme_tests_message();
   };
