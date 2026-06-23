// primitives/meta.cpp -- meta-operations: apply, eval, environment, records, load, etc.
// Direct port of pyscheme/primitives/meta.py.
#include "meta.h"
#include "primitives.h"
#include "../AST.h"
#include "../Context.h"
#include "../Environment.h"
#include "../gc.h"
#include "../Evaluator.h"
#include "../Expander.h"
#include "../Parser.h"
#include "../library.h"
#include "../PrettyPrinter.h"
#include "../process_exec.h"
#include "../dir_list.h"
#include "../unicode_tables.h"
#include "../Utils.h"
#include "../Listener.h"
#include "../Context.h"
#include "../Analyzer.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#ifdef _WIN32
// Declare only the functions we need to avoid pulling in <windows.h>, which would
// conflict with the BOOLEAN constant defined in AST.h.
extern "C"
   {
   __declspec(dllimport) char* __stdcall GetEnvironmentStringsA(void);
   __declspec(dllimport) int __stdcall FreeEnvironmentStringsA(char*);
   __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void*, char*, unsigned long);
   }
#else
#include <unistd.h>
#endif

static const char* CATEGORY = "meta";

static SourceInfo* _src(const Value* a)
   {
   return a ? src_of(*a) : nullptr;
   }

// ── syntax-expand ─────────────────────────────────────────────────────────────

static Value _prim_syntax_expand(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return expand(args[0]);
   }

// ── error object predicates / accessors ───────────────────────────────────────

static Value _prim_file_error_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_error_object(args[0]) && is_file_error_object(args[0]));
   }

static Value _prim_read_error_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_error_object(args[0]) && is_read_error_object(args[0]));
   }

static Value _prim_error_object_message(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_error_object(args[0]))
      throw SchemeTypeError("error-object-message: not an error object", _src(app));
   return make_string(as_error_object_message(args[0]));
   }

static Value _prim_error_object_irritants(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_error_object(args[0]))
      throw SchemeTypeError("error-object-irritants: not an error object", _src(app));
   const auto& irr = as_error_object_irritants(args[0]);
   return list_from_items(std::vector<Value>(irr.begin(), irr.end()));
   }

// ── apply / eval: intercepted by evaluator ────────────────────────────────────

static Value _prim_apply_unreached(Context*, Environment*, std::vector<Value>&, const Value* app)
   {
   throw SchemeTypeError(
       "apply: cannot be called through a re-entering path in this implementation",
       _src(app));
   }

static Value _prim_eval_unreached(Context*, Environment*, std::vector<Value>&, const Value* app)
   {
   throw SchemeTypeError(
       "eval: cannot be called through a re-entering path in this implementation",
       _src(app));
   }

// ── environments ──────────────────────────────────────────────────────────────

static Value _prim_interaction_environment(Context*, Environment* env, std::vector<Value>&, const Value*)
   {
   return make_environment(env->getGlobalEnv());
   }

static Value _prim_environment(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   // Port of meta.py _prim_environment.
   // (environment <library-spec>...) returns a frozen env containing the
   // union of the named libraries' exports.
   Environment* result = gc_alloc_environment(nullptr);
   for (const Value& spec : args)
      {
      if (!is_cons(spec))
         throw SchemeTypeError("environment: argument must be a library-name list", _src(app));
      std::string key;
      try
         {
         key = library_name_to_key(spec);
         }
      catch (const std::exception& e)
         {
         throw SchemeTypeError(std::string("environment: ") + e.what(), _src(app));
         }
      Environment* lib_env = library_lookup(key);
      if (!lib_env)
         {
         throw SchemeTypeError(
             "environment: library not found: " + scheme_pretty_print(spec),
             _src(app));
         }
      for (const auto& [sid, val] : lib_env->_bindings)
         result->bind(symbol_name(sid), val);
      }
   result->freeze();
   return make_environment(result);
   }

// (make-environment <library-spec>...) -- the MUTABLE sibling of R7RS `environment`.
// Returns a fresh top-level environment that allows defines/mutations and isolates
// them.  With NO args it is a child of the global (REPL) environment, so every
// default binding is visible while new top-level defines stay local -- the
// isolation neither `environment` (immutable/frozen) nor `interaction-environment`
// (the single shared global) can give.  With library-specs it holds the union of
// their exports (like `environment`, but not frozen).  Used with `eval` to run a
// program in a clean sandbox (e.g. one ecraven benchmark per fresh env).
// cppScheme2/pyScheme extension.
static Value _prim_make_environment(Context*, Environment* env, std::vector<Value>& args, const Value* app)
   {
   if (args.empty())
      return make_environment(gc_alloc_environment(env->getGlobalEnv()));
   Environment* result = gc_alloc_environment(nullptr);
   for (const Value& spec : args)
      {
      if (!is_cons(spec))
         throw SchemeTypeError("make-environment: argument must be a library-name list", _src(app));
      std::string key;
      try
         {
         key = library_name_to_key(spec);
         }
      catch (const std::exception& e)
         {
         throw SchemeTypeError(std::string("make-environment: ") + e.what(), _src(app));
         }
      Environment* lib_env = library_lookup(key);
      if (!lib_env)
         throw SchemeTypeError(
             "make-environment: library not found: " + scheme_pretty_print(spec), _src(app));
      for (const auto& [sid, val] : lib_env->_bindings)
         result->bind(symbol_name(sid), val);
      }
   return make_environment(result);  // deliberately NOT frozen -> mutable
   }

// (make-toplevel-environment) -> a fresh MUTABLE environment that behaves like a
// freshly rebooted interpreter's global env.  It is a CHILD of the real global (so
// builtins reached via the parent chain, e.g. `apply`, resolve and new defines stay
// isolated); the global's OWN top-level bindings are copied in (so environment
// introspection -- help / apropos, which read getGlobalEnv()._bindings -- sees them);
// and it is rerooted to be its own global (so getGlobalEnv() returns IT, hence code
// run in it sees IT as (interaction-environment)).  The differ's host runner uses one
// per source so meta-circular tests -- e.g. (define x 1) then
// (eval 'x (interaction-environment)) -- resolve against the SAME env, exactly like
// the .log test runner.  cppScheme2/pyScheme extension.
static Value _prim_make_toplevel_environment(Context*, Environment* env, std::vector<Value>&, const Value*)
   {
   Environment* g = env->getGlobalEnv();
   Environment* fresh = gc_alloc_environment(g);          // child of global: chain + isolation
   for (const auto& [sid, val] : g->_bindings)
      fresh->bind(symbol_name(sid), val);                 // copy so help/apropos can introspect
   fresh->reroot_as_global();                             // but is its own global
   return make_environment(fresh);
   }

static Value _prim_null_environment(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_integer(args[0]))
      throw SchemeTypeError("null-environment: version must be an integer", _src(app));
   // R7RS 6.12: version 5 (R5RS) must be supported; if version is neither 5
   // nor another value supported by the implementation, an error is signaled.
   if (as_integer(args[0]) != 5)
      throw SchemeTypeError("null-environment: unsupported version (only 5 is supported)", _src(app));
   Environment* e = gc_alloc_environment(nullptr);
   e->freeze();
   return make_environment(e);
   }

static Value _prim_scheme_report_environment(Context*, Environment* env, std::vector<Value>& args, const Value* app)
   {
   if (!is_integer(args[0]))
      throw SchemeTypeError("scheme-report-environment: version must be an integer", _src(app));
   // R7RS 6.12: only version 5 (R5RS) is required; signal an error otherwise.
   if (as_integer(args[0]) != 5)
      throw SchemeTypeError("scheme-report-environment: unsupported version (only 5 is supported)", _src(app));
   return make_environment(env->getGlobalEnv());
   }

// ── record plumbing ───────────────────────────────────────────────────────────

static Value _prim_make_record_type(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   // (%make-record-type 'name '(field-name ...))
   if (!is_symbol(args[0]))
      throw SchemeTypeError("%make-record-type: first argument must be a symbol", _src(app));
   std::vector<uint32_t> field_ids;
   Value cur = args[1];
   while (is_cons(cur))
      {
      Value f = car(cur);
      if (!is_symbol(f))
         throw SchemeTypeError("%make-record-type: field names must be symbols", _src(app));
      field_ids.push_back(as_symbol_id(f));
      cur = cdr(cur);
      }
   if (!is_nil(cur))
      throw SchemeTypeError("%make-record-type: field names must be a proper list", _src(app));
   return make_record_type(as_symbol(args[0]), field_ids);
   }

static Value _prim_make_record(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   // (%make-record record-type (field-value ...))
   if (!is_record_type(args[0]))
      throw SchemeTypeError("%make-record: first argument must be a record type", _src(app));
   RecordType* rt = as_record_type_obj(args[0]);
   std::vector<Value> values;
   Value cur = args[1];
   while (is_cons(cur))
      {
      values.push_back(car(cur));
      cur = cdr(cur);
      }
   if (!is_nil(cur))
      throw SchemeTypeError("%make-record: field values must be a proper list", _src(app));
   return make_record(rt, std::move(values));
   }

static Value _prim_record_of_type_p(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   // (%record-of-type? obj record-type)
   if (!is_record_type(args[1]))
      throw SchemeTypeError("%record-of-type?: second argument must be a record type", _src(app));
   if (!is_record(args[0]))
      return make_boolean(false);
   return make_boolean(as_record_type(args[0]) == as_record_type_obj(args[1]));
   }

static Value _prim_record_ref(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   // (%record-ref record record-type index)
   if (!is_record_type(args[1]))
      throw SchemeTypeError("%record-ref: second argument must be a record type", _src(app));
   RecordType* rt = as_record_type_obj(args[1]);
   if (!is_record(args[0]) || as_record_type(args[0]) != rt)
      throw SchemeTypeError(
          std::string("%record-ref: record is not of the expected type ") +
              as_record_type_name(args[1]),
          _src(app));
   if (!is_integer(args[2]))
      throw SchemeTypeError("%record-ref: index must be an integer", _src(app));
   int64_t idx = as_integer(args[2]);
   return as_record_fields_const(args[0])[static_cast<size_t>(idx)];
   }

static Value _prim_make_record_accessor(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   // (%make-record-accessor record-type index name)
   if (!is_record_type(args[0]))
      throw SchemeTypeError("%make-record-accessor: first argument must be a record type", _src(app));
   if (!is_integer(args[1]))
      throw SchemeTypeError("%make-record-accessor: index must be an integer", _src(app));
   if (!is_symbol(args[2]))
      throw SchemeTypeError("%make-record-accessor: name must be a symbol", _src(app));
   return make_record_accessor(as_record_type_obj(args[0]),
                               static_cast<int>(as_integer(args[1])),
                               as_symbol(args[2]));
   }

static Value _prim_make_record_mutator(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   // (%make-record-mutator record-type index name)
   if (!is_record_type(args[0]))
      throw SchemeTypeError("%make-record-mutator: first argument must be a record type", _src(app));
   if (!is_integer(args[1]))
      throw SchemeTypeError("%make-record-mutator: index must be an integer", _src(app));
   if (!is_symbol(args[2]))
      throw SchemeTypeError("%make-record-mutator: name must be a symbol", _src(app));
   return make_record_mutator(as_record_type_obj(args[0]),
                              static_cast<int>(as_integer(args[1])),
                              as_symbol(args[2]));
   }

static Value _prim_record_set(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   // (%record-set! record record-type index value)
   if (!is_record_type(args[1]))
      throw SchemeTypeError("%record-set!: second argument must be a record type", _src(app));
   RecordType* rt = as_record_type_obj(args[1]);
   if (!is_record(args[0]) || as_record_type(args[0]) != rt)
      throw SchemeTypeError(
          std::string("%record-set!: record is not of the expected type ") +
              as_record_type_name(args[1]),
          _src(app));
   if (!is_integer(args[2]))
      throw SchemeTypeError("%record-set!: index must be an integer", _src(app));
   int64_t idx = as_integer(args[2]);
   as_record_fields(args[0])[static_cast<size_t>(idx)] = args[3];
   gc_write_barrier(gc_value_header(args[0]), gc_value_header(args[3]));
   return VOID_VALUE;
   }

// ── parameter objects: intercepted by evaluator ───────────────────────────────

static Value _prim_make_parameter_unreached(Context*, Environment*, std::vector<Value>&, const Value* app)
   {
   throw SchemeTypeError(
       "make-parameter: cannot be called through a re-entering path in this implementation",
       _src(app));
   }

static Value _prim_with_parameters_unreached(Context*, Environment*, std::vector<Value>&, const Value* app)
   {
   throw SchemeTypeError(
       "%with-parameters: cannot be called through a re-entering path in this implementation",
       _src(app));
   }

static Value _prim_continuation_depth_unreached(Context*, Environment*, std::vector<Value>&, const Value* app)
   {
   // The Evaluator intercepts %continuation-depth at application dispatch to
   // read the live continuation-stack (K) length, which a normal primitive
   // body cannot see.  This fires only if the interception was bypassed.
   throw SchemeTypeError(
       "%continuation-depth: cannot be called through a re-entering path in this implementation",
       _src(app));
   }

// ── load ──────────────────────────────────────────────────────────────────────

static Value _prim_load(Context* ctx, Environment* env, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("load: filename must be a string", _src(app));
   // R7RS 6.14: (load filename [environment-specifier]).  Evaluate the file's
   // forms in the supplied environment if given, else the current (interaction)
   // environment.
   Environment* eval_env = env;
   if (args.size() >= 2)
      {
      if (!is_environment(args[1]))
         throw SchemeTypeError("load: second argument must be an environment specifier", _src(app));
      eval_env = as_environment(args[1]);
      }
   const std::string path = as_string(args[0]);
   std::ifstream f(path);
   if (!f.is_open())
      throw SchemeFileError("load: cannot open file: " + path, _src(app));
   std::string source((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
   f.close();
   std::vector<Value> forms = scheme_parse(source, path);
   // Root the form list: evaluating an earlier form can GC, which would
   // otherwise evacuate the pending forms' AST and leave dangling pointers.
   GcRootVec forms_root(forms);
   for (const Value& raw : forms)
      {
      Value expanded = expand(raw);
      cek_eval(expanded, eval_env, ctx);
      }
   return VOID_VALUE;
   }

// ── load_setup (PRIM_LOAD) ─────────────────────────────────────────────────────
// Validate (load filename [environment]) and read + parse the file, returning the
// parsed (unexpanded) forms + the evaluation environment.  The evaluator drives
// the forms on the main K stack via FRAME_EVAL_FORMS instead of a re-entrant
// cek_eval per form, so a continuation captured outside the load no longer crosses
// a nested evaluator activation.  Mirrors _prim_load's setup (pyScheme load_setup).
LoadSetup load_setup(std::vector<Value>& args, Environment* env, const Value* app)
   {
   if (args.size() < 1 || args.size() > 2)
      throw SchemeArityError(arity_mismatch_msg("load", 1, 2, (int)args.size()), _src(app));
   if (!is_string(args[0]))
      throw SchemeTypeError("load: filename must be a string", _src(app));
   Environment* eval_env = env;
   if (args.size() >= 2)
      {
      if (!is_environment(args[1]))
         throw SchemeTypeError("load: second argument must be an environment specifier", _src(app));
      eval_env = as_environment(args[1]);
      }
   const std::string path = as_string(args[0]);
   std::ifstream f(path);
   if (!f.is_open())
      throw SchemeFileError("load: cannot open file: " + path, _src(app));
   std::string source((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
   f.close();
   LoadSetup ls;
   ls.forms = scheme_parse(source, path);
   ls.eval_env = eval_env;
   return ls;
   }

// ── process context ───────────────────────────────────────────────────────────

extern int __argc;
extern char** __argv;

static Value _prim_command_line(Context*, Environment*, std::vector<Value>&, const Value*)
   {
   Value result = NIL_VALUE;
   int i = __argc - 1;
   while (i >= 0)
      {
      result = alloc_cons(make_string(__argv[i]), result);
      --i;
      }
   return result;
   }

// (interpreter-executable-path) -- absolute path to the running cppscheme2 binary,
// or #f if it cannot be determined.  Mirrors Listener::_self_exe_path but is exposed
// to Scheme so tests can locate native plugins (e.g. example_plugin.dll) colocated
// with the exe.  cppScheme2-only extension: pyScheme has no single-binary identity,
// and the native .dll plugin path this serves is itself cpp-only.
static std::string _self_exe_path_str()
   {
   char buf[4096];
#ifdef _WIN32
   unsigned long len = GetModuleFileNameA(nullptr, buf, (unsigned long)sizeof(buf));
   if (len > 0 && len < sizeof(buf)) return std::string(buf, len);
#else
   ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf));
   if (len > 0) return std::string(buf, (size_t)len);
#endif
   return std::string();
   }

static Value _prim_interpreter_executable_path(Context*, Environment*, std::vector<Value>&, const Value*)
   {
   std::string exe = _self_exe_path_str();
   return exe.empty() ? make_boolean(false) : make_string(exe);
   }

// (interpreter-argv) -> the argv list that relaunches THIS interpreter, for
// spawning self / sibling interpreters via run-process (e.g. the cli-tests and
// cross-port differential suites).  cppScheme2 is a single exe, so the list has
// one element: (<exe-path>).  A LIST (not a bare path) keeps parity with pyScheme,
// whose relaunch is multi-token (python -m pyscheme).  #f if undeterminable.
static Value _prim_interpreter_argv(Context*, Environment*, std::vector<Value>&, const Value*)
   {
   std::string exe = _self_exe_path_str();
   if (exe.empty()) return make_boolean(false);
   return alloc_cons(make_string(exe), NIL_VALUE);
   }

// (run-process argv [stdin-string]) -> (values exit-code stdout stderr).
// argv is a non-empty list of strings (argv[0] = program, searched on PATH; direct
// exec, NO shell -> arguments are verbatim and injection-safe).  Blocks until the
// child exits.  Optional 2nd arg = a string written to the child's stdin then EOF
// (#f or omitted = empty stdin).  exit-code is the child's status (a negated signal
// number on POSIX signal-kill).  cppScheme2/pyScheme extension; the subprocess
// primitive the de-shelled test arsenal (cli-tests / cross-port diff / fuzz) needs.
// (directory-files path) -> a sorted list of the bare entry names in directory
// PATH, excluding "." and ".." (dotfiles kept).  Models SRFI 170 directory-files;
// names are strings (not full paths) -- join with "/" to build a path.  Errors if
// PATH cannot be opened.  cppScheme2/pyScheme extension.
static Value _prim_directory_files(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("directory-files: argument must be a string", _src(app));
   std::vector<std::string> names;
   if (!list_directory(as_string(args[0]), names))
      throw SchemeTypeError("directory-files: cannot open directory: " + as_string(args[0]),
                            _src(app));
   std::sort(names.begin(), names.end());
   Value result = NIL_VALUE;
   for (auto it = names.rbegin(); it != names.rend(); ++it)
      result = alloc_cons(make_string(*it), result);
   return result;
   }

static Value _prim_run_process(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   std::vector<std::string> argv;
   Value cur = args[0];
   while (is_cons(cur))
      {
      Value head = car(cur);
      if (!is_string(head))
         throw SchemeTypeError("run-process: argv elements must be strings", _src(app));
      argv.push_back(as_string(head));
      cur = cdr(cur);
      }
   if (argv.empty())
      throw SchemeTypeError(
          "run-process: first argument must be a non-empty list of strings", _src(app));
   std::string stdin_buf;
   const std::string* stdin_ptr = nullptr;
   if (args.size() >= 2 && !(is_boolean(args[1]) && !as_boolean(args[1])))
      {
      if (!is_string(args[1]))
         throw SchemeTypeError("run-process: stdin argument must be a string or #f", _src(app));
      stdin_buf = as_string(args[1]);
      stdin_ptr = &stdin_buf;
      }
   double timeout = 0.0;   // <= 0 means no timeout
   if (args.size() >= 3 && !(is_boolean(args[2]) && !as_boolean(args[2])))
      {
      if (!is_number(args[2]))
         throw SchemeTypeError("run-process: timeout must be a number of seconds or #f", _src(app));
      timeout = is_integer(args[2]) ? (double)as_integer(args[2]) : as_real(args[2]);
      }
   ProcessResult res = run_process_blocking(argv, stdin_ptr, timeout);
   if (!res.launched)
      throw SchemeTypeError(res.error, _src(app));
   std::vector<Value> vals;
   // exit-code is #f when the child was killed for exceeding the timeout.
   vals.push_back(res.timed_out ? make_boolean(false) : make_integer(res.exit_code));
   vals.push_back(make_string(res.out));
   vals.push_back(make_string(res.err));
   return make_multi_values(std::move(vals), _src(app));
   }

// (parse-log-file path) -> a list of entries parsed from the .log session-log file
// at PATH.  Each entry is a 5-element list (input output retval error fold-case?):
// the four strings recorded for one REPL cycle plus a boolean fold-case flag.
// Treats a .log file as a reference interpreter for the Scheme-side universal
// interpreter differ.  cppScheme2/pyScheme extension.
static Value _prim_parse_log_file(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("parse-log-file: argument must be a string", _src(app));
   std::ifstream f(as_string(args[0]), std::ios::in);
   if (!f)
      throw SchemeTypeError("parse-log-file: cannot open file: " + as_string(args[0]), _src(app));
   std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
   f.close();
   std::vector<LogEntry> entries = parse_log(text);
   Value result = NIL_VALUE;
   for (auto it = entries.rbegin(); it != entries.rend(); ++it)
      {
      Value entry = NIL_VALUE;
      entry = alloc_cons(make_boolean(it->fold_case), entry);
      entry = alloc_cons(make_string(it->error), entry);
      entry = alloc_cons(make_string(it->retval), entry);
      entry = alloc_cons(make_string(it->output), entry);
      entry = alloc_cons(make_string(it->expr), entry);
      result = alloc_cons(entry, result);
      }
   return result;
   }

// (log-match? expected-output expected-retval expected-error
//             actual-output actual-retval actual-error timed-out?)
// -> #t if the actual output/return/error match the expected (golden) ones under
// the .log test match semantics: '==> X or ==> Y' return alternatives; '%%% *' /
// '%%% %any-error%' accept any raised error; '%%% %optional-error%' models R7RS
// "it is an error" (passes whether or not an error is raised); a timeout always
// fails.  Backs reference-mode comparison in the interpreter differ.
// cppScheme2/pyScheme extension.
static Value _prim_log_match_p(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   for (int i = 0; i < 6; ++i)
      if (!is_string(args[i]))
         throw SchemeTypeError("log-match?: arguments 1-6 must be strings", _src(app));
   // Scheme truthiness: only #f is false.
   bool timed_out = !(is_boolean(args[6]) && !as_boolean(args[6]));
   LogMatch m = log_match(as_string(args[0]), as_string(args[1]), as_string(args[2]),
                          as_string(args[3]), as_string(args[4]), as_string(args[5]),
                          timed_out);
   return make_boolean(m.output_ok && m.retval_ok && m.error_ok);
   }

// (log-match-detail exp-output exp-retval exp-error
//                   act-output act-retval act-error timed-out?)
// -> the 3-element list (output-ok? retval-ok? error-ok?) of per-channel match
// results under the SAME .log semantics as log-match? (which is their AND), so a
// caller can report WHICH channel diverged (e.g. a per-channel .run report).
// cppScheme2/pyScheme extension.
static Value _prim_log_match_detail(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   for (int i = 0; i < 6; ++i)
      if (!is_string(args[i]))
         throw SchemeTypeError("log-match-detail: arguments 1-6 must be strings", _src(app));
   bool timed_out = !(is_boolean(args[6]) && !as_boolean(args[6]));
   LogMatch m = log_match(as_string(args[0]), as_string(args[1]), as_string(args[2]),
                          as_string(args[3]), as_string(args[4]), as_string(args[5]),
                          timed_out);
   return list_from_items({make_boolean(m.output_ok), make_boolean(m.retval_ok),
                           make_boolean(m.error_ok)});
   }

// Right-strip trailing whitespace (the .log runner rstrips captured output).
static std::string _ec_rstrip(const std::string& s)
   {
   size_t end = s.size();
   while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                      s[end - 1] == '\r' || s[end - 1] == '\n'))
      --end;
   return s.substr(0, end);
   }

// (eval-cycle input env [timeout-secs]) -> (values output retval error timed-out?)
// Evaluate the Scheme source string INPUT as ONE REPL cycle in environment ENV (a
// make-environment env; state persists across cycles), capturing exactly what the
// existing .log test runner captures: standard output, the REPL-formatted return
// value (write form; void -> ""; multiple values space-joined), and -- on error --
// the SAME formatted error text the runner records (exception class + source
// location + message, via Listener::format_error).  Optional 3rd arg = a timeout in
// seconds (#f/omitted = none); on timeout the error names the timeout and the 4th
// value is #t.  This is the host port's IN-PROCESS live runner for the interpreter
// differ: it shares the runner's parse/expand/eval/format path so results are
// byte-identical (parse->expand->cek_eval; analyze is a checking pass cek_eval does
// not need, mirroring the eval primitive).  cppScheme2/pyScheme extension.
static Value _prim_eval_cycle(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("eval-cycle: first argument must be a string", _src(app));
   if (!is_environment(args[1]))
      throw SchemeTypeError("eval-cycle: second argument must be an environment", _src(app));
   Environment* target = as_environment(args[1]);
   std::string source = as_string(args[0]);

   bool have_timeout = false;
   double timeout_secs = 0.0;
   if (args.size() >= 3 && !(is_boolean(args[2]) && !as_boolean(args[2])))
      {
      if (!is_number(args[2]))
         throw SchemeTypeError("eval-cycle: timeout must be a number of seconds or #f", _src(app));
      timeout_secs = is_integer(args[2]) ? (double)as_integer(args[2]) : as_real(args[2]);
      have_timeout = true;
      }

   // eval-cycle runs NESTED inside the differ's own evaluation, so save every bit of
   // ctx we transiently repurpose and restore it afterwards -- the outer evaluation
   // must be left pristine.
   std::ostream* prev_out = ctx->outStrm;
   SteadyTimePoint prev_deadline = ctx->timeout_at;
   bool prev_timeout_active = ctx->timeout_active;
   std::vector<ShadowEntry> saved_shadow = ctx->shadow_stack;
   std::vector<Value> saved_handlers = ctx->handler_stack;

   std::ostringstream out_capture;
   ctx->outStrm = &out_capture;
   ctx->shadow_stack.clear();
   if (have_timeout)
      {
      ctx->timeout_at = SteadyClock::now() +
          std::chrono::milliseconds((long long)(timeout_secs * 1000.0));
      ctx->timeout_active = true;
      }

   std::string retval;
   std::string errstr;
   bool timed_out = false;
   try
      {
      std::vector<Value> forms = scheme_parse(source, "");
      GcRootVec forms_root(forms);
      // Mirror Interpreter::rawEval: analyze (a checking pass -- its result is
      // discarded; cek_eval runs the un-analyzed form) then accumulate user defines
      // so cross-form arity checks fire, exactly as the .log runner does.  The static
      // env is seeded with the primitive/special-form arities.
      StaticEnv senv(primitive_arities());
      Value last = NIL_VALUE;
      bool have = false;
      for (auto& form : forms)
         {
         Value expanded = expand(form);
         analyze(expanded, senv);
         extend_static_env_with_define(senv, expanded);
         last = cek_eval(expanded, target, ctx);
         have = true;
         }
      if (have && !is_void(last))
         {
         if (is_multi_values(last))
            {
            const std::vector<Value>& vs = as_multi_values_list(last);
            for (size_t i = 0; i < vs.size(); ++i)
               {
               if (i)
                  retval += ' ';
               retval += scheme_pretty_print(vs[i]);
               }
            }
         else
            retval = scheme_pretty_print(last);
         }
      }
   catch (ReplExitSignal&)
      {
      // A test that calls (exit): record a token instead of aborting the differ.
      errstr = "(exit)";
      }
   catch (std::exception& e)
      {
      errstr = Listener::format_error(e);
      if (errstr.find("Evaluation timed out.") != std::string::npos)
         timed_out = true;
      }

   ctx->outStrm = prev_out;
   ctx->timeout_at = prev_deadline;
   ctx->timeout_active = prev_timeout_active;
   ctx->shadow_stack = saved_shadow;
   ctx->handler_stack = saved_handlers;

   std::vector<Value> vals;
   vals.push_back(make_string(_ec_rstrip(out_capture.str())));
   vals.push_back(make_string(retval));
   vals.push_back(make_string(errstr));
   vals.push_back(make_boolean(timed_out));
   return make_multi_values(std::move(vals), _src(app));
   }

static Value _prim_exit(Context* ctx, Environment*, std::vector<Value>& args, const Value*)
   {
   int code;
   if (args.empty())
      code = 0;
   else if (is_boolean(args[0]))
      code = as_boolean(args[0]) ? 0 : 1;
   else if (is_integer(args[0]))
      code = static_cast<int>(as_integer(args[0]));
   else
      code = 1;
   // In a live REPL session (exit) aborts the current evaluation and returns to
   // the '>>> ' prompt rather than terminating the process; batch file
   // execution still exits the process.  See Context::interactive.
   if (ctx != nullptr && ctx->interactive)
      throw ReplExitSignal{code};
   std::exit(code);
   }

static Value _prim_emergency_exit(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   // Mirror Python os._exit(): bypass atexit / port flush.
   if (args.empty())
      std::_Exit(0);
   if (is_boolean(args[0]))
      std::_Exit(as_boolean(args[0]) ? 0 : 1);
   if (is_integer(args[0]))
      std::_Exit(static_cast<int>(as_integer(args[0])));
   std::_Exit(1);
   }

static Value _prim_get_environment_variable(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("get-environment-variable: argument must be a string", _src(app));
   const char* val = std::getenv(as_string(args[0]).c_str());
   if (!val)
      return make_boolean(false);
   return make_string(std::string(val));
   }

static Value _prim_get_environment_variables(Context*, Environment*, std::vector<Value>&, const Value*)
   {
#ifdef _WIN32
   // Use GetEnvironmentStrings to avoid DLL-linkage issues with _environ.
   char* block = GetEnvironmentStringsA();
   if (!block)
      return NIL_VALUE;
   std::vector<std::pair<std::string, std::string>> entries;
   for (char* p = block; *p; p += strlen(p) + 1)
      {
      std::string entry(p);
      auto eq = entry.find('=');
      if (eq == 0)
         continue; // skip hidden Win32 env vars like "=C:=..."
      std::string k = (eq != std::string::npos) ? entry.substr(0, eq) : entry;
      std::string v = (eq != std::string::npos) ? entry.substr(eq + 1) : std::string();
      // Port of Python os.environ on Windows: normalize key to uppercase.
      for (char& c : k)
         c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
      entries.push_back({std::move(k), std::move(v)});
      }
   FreeEnvironmentStringsA(block);
   Value result = NIL_VALUE;
   for (int i = (int)entries.size() - 1; i >= 0; --i)
      {
      Value pair = alloc_cons(make_string(entries[i].first), make_string(entries[i].second));
      result = alloc_cons(pair, result);
      }
   return result;
#else
   extern char** environ;
   Value result = NIL_VALUE;
   if (!environ)
      return result;
   int count = 0;
   while (environ[count])
      ++count;
   for (int i = count - 1; i >= 0; --i)
      {
      std::string entry(environ[i]);
      auto eq = entry.find('=');
      std::string k = (eq != std::string::npos) ? entry.substr(0, eq) : entry;
      std::string v = (eq != std::string::npos) ? entry.substr(eq + 1) : std::string();
      Value pair = alloc_cons(make_string(k), make_string(v));
      result = alloc_cons(pair, result);
      }
   return result;
#endif
   }

// ── time ──────────────────────────────────────────────────────────────────────

static Value _prim_unicode_version(Context*, Environment*, std::vector<Value>&, const Value*)
   {
   return make_string(std::string(unicode::version()));
   }

static Value _prim_runtime(Context*, Environment*, std::vector<Value>&, const Value*)
   {
   return make_real(static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC));
   }

static Value _prim_current_second(Context*, Environment*, std::vector<Value>&, const Value*)
   {
   using namespace std::chrono;
   auto now = system_clock::now();
   return make_real(duration_cast<duration<double>>(now.time_since_epoch()).count());
   }

static Value _prim_current_jiffy(Context*, Environment*, std::vector<Value>&, const Value*)
   {
   using namespace std::chrono;
   auto now = steady_clock::now();
   auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
   return make_integer(static_cast<int64_t>(ms));
   }

static Value _prim_jiffies_per_second(Context*, Environment*, std::vector<Value>&, const Value*)
   {
   return make_integer(1000);
   }

// ── registration ──────────────────────────────────────────────────────────────

void register_meta()
   {
   register_primitive("apply", 2, -1, _prim_apply_unreached,
                      "(apply <proc> <arg>... <list>)",
                      "Call <proc> with the elements of <list> as its arguments, optionally "
                      "prepended by any leading <arg>s.  The last argument must be a proper list.  R7RS 6.10.",
                      CATEGORY);

   register_primitive("eval", 1, 2, _prim_eval_unreached,
                      "(eval <datum> [<env-spec>])",
                      "Evaluate <datum> as a Scheme expression in the given or global environment.  R7RS 6.12.",
                      CATEGORY);

   register_primitive("interaction-environment", 0, 0, _prim_interaction_environment,
                      "(interaction-environment)",
                      "Return a specifier for the current REPL / top-level environment (mutable).  R7RS 6.12.",
                      CATEGORY);

   register_primitive("environment", 0, -1, _prim_environment,
                      "(environment <library-spec>...)",
                      "Return a frozen environment built from named libraries' exports.  R7RS 6.12.",
                      CATEGORY);

   register_primitive("make-environment", 0, -1, _prim_make_environment,
                      "(make-environment <library-spec>...)",
                      "Return a fresh MUTABLE top-level environment (the mutable sibling of R7RS "
                      "`environment`).  No args: a child of the global REPL environment (all default "
                      "bindings visible, new defines isolated).  With library-specs: the union of "
                      "their exports, not frozen.  Use with `eval` to run a program in isolation.  "
                      "cppScheme2/pyScheme extension.", CATEGORY);

   register_primitive("make-toplevel-environment", 0, 0, _prim_make_toplevel_environment,
                      "(make-toplevel-environment)",
                      "Return a fresh MUTABLE environment that is its OWN global -- like "
                      "make-environment but self-rooted, so code run in it sees IT as "
                      "(interaction-environment).  Populated with every current global top-level "
                      "binding, so all builtins are present.  Equivalent to a freshly rebooted "
                      "interpreter's global env; used by the interpreter differ's host runner so "
                      "meta-circular tests resolve against the same env.  cppScheme2/pyScheme extension.",
                      CATEGORY);

   register_primitive("%make-record-type", 2, 2, _prim_make_record_type,
                      "", "Build a record-type descriptor.  Internal: used by define-record-type.", CATEGORY);

   register_primitive("%make-record", 2, 2, _prim_make_record,
                      "", "Build a record of a given type from a field-values list.  Internal.", CATEGORY);

   register_primitive("%record-of-type?", 2, 2, _prim_record_of_type_p,
                      "", "#t if obj is a record of the given record type.  Internal.", CATEGORY);

   register_primitive("%record-ref", 3, 3, _prim_record_ref,
                      "", "Read the i-th field of a record; type-checks against the given record-type.  Internal.",
                      CATEGORY);

   register_primitive("%record-set!", 4, 4, _prim_record_set,
                      "", "Mutate the i-th field of a record; type-checks against the given record-type.  Internal.",
                      CATEGORY);

   register_primitive("%make-record-accessor", 3, 3, _prim_make_record_accessor,
                      "", "Build a record accessor value.  Internal: emitted by define-record-type.", CATEGORY);

   register_primitive("%make-record-mutator", 3, 3, _prim_make_record_mutator,
                      "", "Build a record mutator value.  Internal: emitted by define-record-type.", CATEGORY);

   register_primitive("make-parameter", 1, 2, _prim_make_parameter_unreached,
                      "(make-parameter <init> [<converter>])",
                      "Return a new parameter object.  Intercepted by evaluator.  R7RS 4.2.6.", CATEGORY);

   register_primitive("%with-parameters", 3, 3, _prim_with_parameters_unreached,
                      "", "Dynamically bind parameters for the extent of a thunk.  Internal.", CATEGORY);

   register_primitive("%continuation-depth", 0, 0, _prim_continuation_depth_unreached,
                      "", "Return the current continuation-stack (K) length.  Internal: used by "
                      "tail-call tests to assert bounded continuation space.  Intercepted by the "
                      "Evaluator (a normal primitive body cannot see K).", CATEGORY);

   register_primitive("error-object-message", 1, 1, _prim_error_object_message,
                      "", "Return the message string of an error object.  R7RS 6.11.", CATEGORY);

   register_primitive("error-object-irritants", 1, 1, _prim_error_object_irritants,
                      "", "Return the irritants list of an error object.  R7RS 6.11.", CATEGORY);

   register_primitive("file-error?", 1, 1, _prim_file_error_p,
                      "", "(file-error? obj) returns #t if obj is a file-related error.  R7RS 6.11.", CATEGORY);

   register_primitive("read-error?", 1, 1, _prim_read_error_p,
                      "", "(read-error? obj) returns #t if obj is a read/parse error.  R7RS 6.11.", CATEGORY);

   register_primitive("null-environment", 1, 1, _prim_null_environment,
                      "", "(null-environment version) returns a minimal frozen environment.  R7RS 6.12.", CATEGORY);

   register_primitive("scheme-report-environment", 1, 1, _prim_scheme_report_environment,
                      "", "(scheme-report-environment version) returns an environment with standard procedures.  R7RS 6.12.",
                      CATEGORY);

   register_primitive("load", 1, 2, _prim_load,
                      "", "(load filename) reads and evaluates all forms in filename.  R7RS 6.14.", CATEGORY);

   register_primitive("command-line", 0, 0, _prim_command_line,
                      "", "(command-line) returns the command-line arguments as a list of strings.  R7RS 6.14.",
                      CATEGORY);

   register_primitive("interpreter-executable-path", 0, 0, _prim_interpreter_executable_path,
                      "(interpreter-executable-path)",
                      "Return the absolute path to the running cppscheme2 executable image, or #f if "
                      "it cannot be determined.  cppScheme2 extension: lets a test locate native "
                      "plugins (e.g. example_plugin.dll) colocated with the exe.",
                      CATEGORY);

   register_primitive("interpreter-argv", 0, 0, _prim_interpreter_argv,
                      "(interpreter-argv)",
                      "Return the argv list (of strings) that relaunches THIS interpreter, for "
                      "spawning self / sibling interpreters via run-process.  cppScheme2: a one-element "
                      "list (the exe path); pyScheme: (python -m pyscheme).  #f if undeterminable.  "
                      "cppScheme2/pyScheme extension.", CATEGORY);

   register_primitive("directory-files", 1, 1, _prim_directory_files,
                      "(directory-files path)",
                      "Return a sorted list of the bare filenames in directory PATH, excluding "
                      "\".\" and \"..\" (dotfiles kept).  Names are strings, not full paths -- join "
                      "with \"/\" to build a path (\"/\" works as an OS path separator on Windows "
                      "too).  Errors if PATH cannot be opened.  Models SRFI 170 directory-files.  "
                      "cppScheme2/pyScheme extension.", CATEGORY);

   register_primitive("run-process", 1, 3, _prim_run_process,
                      "(run-process argv [stdin-string [timeout-secs]])",
                      "Run argv (a non-empty list of strings; argv[0] searched on PATH) as a child "
                      "process with NO shell, blocking until it exits.  Optional 2nd arg = a string "
                      "written to the child's stdin; optional 3rd arg = a timeout in seconds (a "
                      "number; #f or omitted = no limit) after which the child is killed.  Returns "
                      "THREE values: exit-code (a negated signal number on POSIX signal-kill, or #f "
                      "if it timed out), captured stdout string, captured stderr string.  "
                      "cppScheme2/pyScheme extension.", CATEGORY);

   register_primitive("parse-log-file", 1, 1, _prim_parse_log_file,
                      "(parse-log-file path)",
                      "Parse the .log session-log file at PATH into a list of entries.  Each entry "
                      "is a 5-element list (input output retval error fold-case?): the input "
                      "expression, recorded output, return value and error message (strings) for one "
                      "REPL cycle, plus a fold-case boolean.  Treats a .log file as a reference "
                      "interpreter for the universal interpreter differ.  cppScheme2/pyScheme extension.",
                      CATEGORY);

   register_primitive("log-match?", 7, 7, _prim_log_match_p,
                      "(log-match? exp-output exp-retval exp-error act-output act-retval act-error timed-out?)",
                      "Return #t if the actual output/return/error (args 4-6) match the expected "
                      "golden ones (args 1-3) under the .log test match semantics: '==> X or ==> Y' "
                      "return alternatives; '%%% *' / '%%% %any-error%' accept any raised error; "
                      "'%%% %optional-error%' models R7RS \"it is an error\" (passes either way); a "
                      "true TIMED-OUT? always fails.  Args 1-6 are strings.  cppScheme2/pyScheme extension.",
                      CATEGORY);

   register_primitive("log-match-detail", 7, 7, _prim_log_match_detail,
                      "(log-match-detail exp-output exp-retval exp-error act-output act-retval act-error timed-out?)",
                      "Like log-match?, but return the per-channel list (output-ok? retval-ok? error-ok?) "
                      "instead of their AND, so a caller can report WHICH channel diverged (e.g. a "
                      "per-channel .run report).  Same .log match semantics and string arguments.  "
                      "cppScheme2/pyScheme extension.",
                      CATEGORY);

   register_primitive("eval-cycle", 2, 3, _prim_eval_cycle,
                      "(eval-cycle input env [timeout-secs])",
                      "Evaluate the Scheme source string INPUT as one REPL cycle in environment ENV "
                      "(typically a make-environment env; state persists across cycles).  Returns "
                      "FOUR values: captured standard output, the REPL-formatted return value (write "
                      "form; void -> \"\"; multiple values space-joined), the formatted error text "
                      "(exception class + source location + message, exactly as the .log test runner "
                      "records it) or \"\" if none, and a timed-out? boolean.  Optional 3rd arg = a "
                      "timeout in seconds (#f or omitted = none).  The host port's in-process live "
                      "runner for the interpreter differ.  cppScheme2/pyScheme extension.",
                      CATEGORY);

   register_primitive("exit", 0, 1, _prim_exit,
                      "", "(exit [obj]) exits the process.  R7RS 6.14.", CATEGORY);

   register_primitive("emergency-exit", 0, 1, _prim_emergency_exit,
                      "", "(emergency-exit [obj]) terminates immediately without cleanup.  R7RS 6.14.", CATEGORY);

   register_primitive("get-environment-variable", 1, 1, _prim_get_environment_variable,
                      "", "(get-environment-variable name) returns the OS env-var value as a string, or #f.  R7RS 6.14.",
                      CATEGORY);

   register_primitive("get-environment-variables", 0, 0, _prim_get_environment_variables,
                      "", "(get-environment-variables) returns an alist of (name . value) strings.  R7RS 6.14.",
                      CATEGORY);

   register_primitive("unicode-version", 0, 0, _prim_unicode_version,
                      "", "(unicode-version) returns the version string of the Unicode character "
                      "database backing char/string operations (e.g. \"16.0.0\").  Not in R7RS.",
                      CATEGORY);

   register_primitive("syntax-expand", 1, 1, _prim_syntax_expand,
                      "(syntax-expand form)",
                      "Expand a Scheme form through the macro/sugar expander and return the result.  Non-standard.",
                      CATEGORY);

   register_primitive("runtime", 0, 0, _prim_runtime,
                      "", "(runtime) returns CPU process time in seconds.  MIT Scheme compat.", CATEGORY);

   register_primitive("current-second", 0, 0, _prim_current_second,
                      "", "(current-second) returns current UTC time as inexact real seconds since epoch.  R7RS 6.14.",
                      CATEGORY);

   register_primitive("current-jiffy", 0, 0, _prim_current_jiffy,
                      "", "(current-jiffy) returns monotonic millisecond count.  R7RS 6.14.", CATEGORY);

   register_primitive("jiffies-per-second", 0, 0, _prim_jiffies_per_second,
                      "", "(jiffies-per-second) returns 1000: jiffies are milliseconds.  R7RS 6.14.", CATEGORY);
   }
