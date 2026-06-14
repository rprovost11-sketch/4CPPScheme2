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
#include "../unicode_tables.h"
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <string>
#ifdef _WIN32
// Declare only the env-string functions we need to avoid pulling in <windows.h>,
// which would conflict with the BOOLEAN constant defined in AST.h.
extern "C"
   {
   __declspec(dllimport) char* __stdcall GetEnvironmentStringsA(void);
   __declspec(dllimport) int __stdcall FreeEnvironmentStringsA(char*);
   }
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
