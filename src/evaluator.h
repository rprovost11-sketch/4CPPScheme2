#pragma once
// Evaluator.h -- CEK machine evaluator.
// Direct port of pyscheme/Evaluator.py.
#include "AST.h"
#include "Context.h"
#include "Environment.h"
#include "scheme_export.h"
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
