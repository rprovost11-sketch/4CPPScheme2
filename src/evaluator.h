#pragma once
// Evaluator.h -- CEK machine evaluator.
// Direct port of pyscheme/Evaluator.py.
#include "AST.h"
#include "Context.h"
#include "Environment.h"
#include "scheme_export.h"
#include <exception>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// ── Frame tag constants ───────────────────────────────────────────────────────
// Direct port of Evaluator.py FRAME_* constants.

constexpr int FRAME_DEFINE = 0;
constexpr int FRAME_SET = 1;
constexpr int FRAME_IF = 2;
constexpr int FRAME_ARG = 3;
constexpr int FRAME_CALL = 4;
constexpr int FRAME_SEQ = 5;
constexpr int FRAME_WHEN = 6;
constexpr int FRAME_UNLESS = 7;
constexpr int FRAME_AND = 8;
constexpr int FRAME_OR = 9;
constexpr int FRAME_COND = 10;
constexpr int FRAME_COND_ARROW = 11;
constexpr int FRAME_LET = 12;
constexpr int FRAME_LET_STAR = 13;
constexpr int FRAME_LETREC = 14;
constexpr int FRAME_CASE = 15;
constexpr int FRAME_DYNAMIC_WIND_AFTER = 16;
constexpr int FRAME_CWV_CONSUMER = 17;
constexpr int FRAME_FORCE_RESULT = 18;
constexpr int FRAME_MAKE_PARAMETER = 19;
constexpr int FRAME_POP_HANDLER = 20;
constexpr int FRAME_REINSTALL_HANDLER = 21;
constexpr int FRAME_CASE_ARROW = 22;
constexpr int FRAME_SHADOW_POP = 23;
constexpr int FRAME_TRACE_EXIT = 24;
constexpr int FRAME_NONCONTIN_RETURN = 25;
constexpr int FRAME_GUARD = 26;
constexpr int FRAME_HOF_STEP = 27;       // map / for-each / filter driver
constexpr int FRAME_HOF_STEP_IDX = 28;   // vector/string -map / -for-each driver
constexpr int FRAME_SEARCH_STEP = 29;    // member / assoc 3-arg comparator driver
constexpr int FRAME_RESTORE_VALUE = 30;          // reinstate a saved V after a wind thunk
constexpr int FRAME_DYNAMIC_WIND_BEFORE_DONE = 31; // dynamic-wind before-thunk completed
constexpr int FRAME_PARAMETERIZE_STEP = 32;      // parameterize value-converter driver
constexpr int FRAME_WIND_STEP = 33;              // continuation-jump wind walk driver
constexpr int FRAME_ERROR_UNWIND = 34;           // error-unwind after-thunk driver
constexpr int FRAME_EVAL_FORMS = 35;             // evaluate a form list on the K stack
constexpr int FRAME_LIB_FINALIZE = 36;           // build + register a define-library's exports
constexpr int FRAME_IMPORT_STEP = 37;            // resolve + bind import sets on the K stack
constexpr int FRAME_ENSURE_LOADED = 38;          // load-path search + .sld eval on the K stack

// ── Frame struct (runtime continuation entries) ───────────────────────────────
// Port of Evaluator.py frame tuples.  Each tag uses a subset of fields:
//
//   FRAME_DEFINE:               v1=name_sym,       env=env
//   FRAME_SET:                  v1=name_sym,        env=env,  src_ptr=name_src
//   FRAME_IF:                   v1=then_br, v2=else_br, env=env
//   FRAME_ARG:                  list1=args,         env=saved_env, v1=app_node
//   FRAME_CALL:                 v1=fn_val, list1=collected, list2=remaining,
//                                 env=saved_env, v2=app_node
//   FRAME_SEQ/WHEN/UNLESS/AND/OR: v1=body/remaining_cons, env=env
//   FRAME_COND/CASE:            v1=current_clause, v2=remaining_clauses, env=env
//   FRAME_COND_ARROW/CASE_ARROW: v1=test/key_value, env=env
//   FRAME_LET:                  ids=names, list1=collected, list2=remaining_vals,
//                                 v1=body_cons, env=saved_env
//   FRAME_LET_STAR/LETREC:      uid=name_id, pairs=remaining_pairs,
//                                 v1=body_cons, env=env
//   FRAME_DYNAMIC_WIND_AFTER:   v1=after_thunk
//   FRAME_CWV_CONSUMER:         v1=consumer, v2=app_node
//   FRAME_FORCE_RESULT:         v1=promise
//   FRAME_MAKE_PARAMETER:       v1=converter
//   FRAME_POP_HANDLER:          (empty)
//   FRAME_REINSTALL_HANDLER:    v1=handler
//   FRAME_SHADOW_POP:           (empty)
//   FRAME_TRACE_EXIT:           str1=fn_name, depth=depth
//   FRAME_NONCONTIN_RETURN:     v1=raised_value
//   FRAME_GUARD:                (empty) -- like FRAME_POP_HANDLER but for %guard-eval
//   FRAME_HOF_STEP:             v1=proc, v2=acc (reversed cons), list1=cursors
//                                 (cons cells, one per list), list2={app_node, pending},
//                                 depth=mode (PRIM_MAP/FOR_EACH/FILTER), uid=started
//   FRAME_HOF_STEP_IDX:         v1=proc, v2=acc (reversed cons), list1=seqs (the
//                                 vector/string values), list2={app_node},
//                                 ids=positions (one per seq; byte offset for strings,
//                                 element index for vectors), depth=mode, uid=started
//   FRAME_SEARCH_STEP:          v1=proc, v2=target, list1={cursor} (the cons cell
//                                 currently under test), list2={app_node},
//                                 depth=mode (PRIM_MEMBER/PRIM_ASSOC), uid=started
//   FRAME_RESTORE_VALUE:        v1=saved_value (discard incoming V, reinstate v1)
//   FRAME_DYNAMIC_WIND_BEFORE_DONE: v1=before, v2=thunk, list1={after}
//   FRAME_PARAMETERIZE_STEP:    list1=params, list2=raw_vals, pairs=acc (running
//                                 converted values, in .second; .first unused),
//                                 v1=thunk, v2=app_node, depth=index, uid=awaiting(0/1)
//   FRAME_WIND_STEP:            list1=op_thunks (per op: exit=after, enter=before),
//                                 list2=op_after (the entry.after to push for an
//                                 enter op; NIL for an exit op), ids=op_kinds
//                                 (0=exit, 1=enter), v1=cont, v2=cont_value, depth=index
//   FRAME_ERROR_UNWIND:         list1=afters, depth=index, exc=original exception_ptr
//                                 (re-raised once the afters are exhausted)
//   FRAME_EVAL_FORMS:           list1=forms (parsed, unexpanded), env=eval_env,
//                                 depth=index (each form expanded then evaluated;
//                                 result discarded; yields VOID when exhausted)
//   FRAME_LIB_FINALIZE:         env=lib_env, v1=def_lib_form (for error src),
//                                 pairs=export_names ((internal_sid, external symbol
//                                 Value)), str1=key
//   FRAME_IMPORT_STEP:          list1=import_sets, depth=index, env=bind_env,
//                                 uid=post_load(0/1), str1=err_prefix
//   FRAME_ENSURE_LOADED:        str1=key, v1=name_sexpr, depth=dir_index
//                                 (the load-path dirs are recomputed each step via
//                                 _library_load_path(); .sld forms driven via
//                                 FRAME_EVAL_FORMS)
// (All HOF Values live in v1/v2/list1/list2 so they are GC-traced by the existing
//  frame trace/forward; the int state lives in depth/uid/ids, which need no tracing.)

struct Frame
   {
   int tag = 0;
   Value v1;
   Value v2;
   Environment* env = nullptr;
   SourceInfo* src_ptr = nullptr;
   std::vector<Value> list1;
   std::vector<Value> list2;
   std::vector<uint32_t> ids;
   std::vector<std::pair<uint32_t, Value>> pairs;
   uint32_t uid = 0;
   int depth = 0;
   std::string str1;
   // FRAME_ERROR_UNWIND only: the original in-flight exception, captured via
   // std::current_exception() in the handler-dispatch scan and re-raised once the
   // collected after-thunks have run (propagate semantics).  Null for all other
   // frames.  Not GC-relevant (holds a C++ exception, not a Value).
   std::exception_ptr exc;
   };

using KStack = std::vector<Frame>;

// ── Public API ────────────────────────────────────────────────────────────────
// Port of Evaluator.py cek_eval, set_global_env, _sorted_sym_list.

// Evaluate expr against env.  ctx may be nullptr (a default Context is created).
CPPSCHEME2_API Value cek_eval(const Value& expr, Environment* env, Context* ctx = nullptr);

// Global env reference for the library loader / eval primitive.
CPPSCHEME2_API void set_global_env(Environment* env);
CPPSCHEME2_API Environment* get_global_env();

// Synchronous procedure call: applies fn(args) via cek_eval.
// Port of primitives/meta.py _apply_scheme_proc.
// app_node may be nullptr.
CPPSCHEME2_API Value apply_scheme_proc(const Value& fn, std::vector<Value> args,
                                       Context* ctx, Environment* env,
                                       const Value* app_node);

// Build a sorted Scheme proper list of symbols from a name set.
// Port of Evaluator.py _sorted_sym_list.
CPPSCHEME2_API Value sorted_sym_list(const std::unordered_set<std::string>& fns);

// Process top-level (import ...) form.  Binds exported names into env.
// Port of Evaluator.py _process_import.
CPPSCHEME2_API void process_import(const Value& sets_cons, Environment* env, Context* ctx);

// ── Port-runner setup (PRIM_PORT_RUNNER) ──────────────────────────────────────
// Port of pyScheme primitives/ports.py port_runner_setup.  Defined in
// primitives/ports.cpp (needs port internals).  Validates and opens as the
// _prim_* bodies did, then returns the pieces the evaluator pushes: a
// dynamic-wind entry (before, after) -- after is a GC-managed native closure
// that closes the port and, for the with-* forms, restores the current-port
// parameter -- plus the body proc/thunk to tail-call (body_args is {port} for
// call-with-* and {} for the with-* thunk forms).
struct PortRunnerSetup
   {
   Value before;
   Value after;
   Value body_proc;
   std::vector<Value> body_args;
   };
CPPSCHEME2_API PortRunnerSetup port_runner_setup(const std::string& name,
                                                 Context* ctx, Environment* env,
                                                 std::vector<Value>& args,
                                                 const Value* app_node);

// ── load setup (PRIM_LOAD) ────────────────────────────────────────────────────
// Port of pyScheme primitives/meta.py load_setup.  Defined in primitives/meta.cpp.
// Validates (load filename [environment]) and reads + parses the file, returning
// the parsed top-level forms + the evaluation environment.  The evaluator drives
// the forms on the main K stack via FRAME_EVAL_FORMS instead of a re-entrant
// cek_eval per form.
struct LoadSetup
   {
   std::vector<Value> forms;
   Environment* eval_env = nullptr;
   };
CPPSCHEME2_API LoadSetup load_setup(std::vector<Value>& args, Environment* env,
                                    const Value* app_node);
