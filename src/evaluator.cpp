// Evaluator.cpp -- CEK machine evaluator.
// Direct port of pyscheme/Evaluator.py.
#include "Evaluator.h"
#include "Debugger.h"
#include "AST.h"
#include "Analyzer.h"
#include "Context.h"
#include "Environment.h"
#include "Expander.h"
#include "Parser.h"
#include "PrettyPrinter.h"
#include "Tracer.h"
#include "library.h"
#include "gc.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace fs = std::filesystem;

// ── Module globals ─────────────────────────────────────────────────────────────

static Environment* g_global_env = nullptr;

static constexpr int SHADOW_DEPTH_LIMIT = 50;

// ── Helper structs ────────────────────────────────────────────────────────────

struct BetaResult
   {
   Environment* new_env;
   Value body;
   };

struct EnterResult
   {
   enum Kind
      {
      IsValue,
      IsEnter,
      IsFrame      // higher-order primitive reached as a callback: push `frame`
                   // on K and resume the APPLY loop (drives the call on frames
                   // instead of re-entering cek_eval).  Mirrors pyScheme's
                   // ('frame', ...) descriptor.
      } kind = IsValue;
   Value v;
   Environment* new_env = nullptr;
   Value seq; // IsEnter: remaining body cons or NIL_VALUE
   Frame frame; // IsFrame: the driver frame to push onto K
   };

struct CondClause
   {
   enum Kind
      {
      Else,
      Arrow,
      TestOnly,
      Body
      } kind;
   Value test;
   Value proc_expr;
   Value body_cons;
   };

// ── Forward declarations ───────────────────────────────────────────────────────

static BetaResult beta_reduce_core(const std::vector<uint32_t>& params, const Value& body,
                                   Environment* clo_env, uint32_t rest_id,
                                   const std::vector<Value>& arg_values, const Value* app_node);
static BetaResult apply_value(const Value& V, const std::vector<Value>& arg_values,
                              const Value* app_node);
static EnterResult enter_proc(const Value& fn_value, std::vector<Value>& args,
                              Context* ctx, Environment* saved_env, const Value* app_node);
static void unwind_winds_on_error(Context* ctx, size_t target_depth);
static std::pair<std::vector<Value>, std::vector<Value>> resolve_parameterize_params(
    const Value& params_list, const Value& values_list, Context* ctx, const Value* app_node);
static std::pair<Value, Value> finalize_parameterize_winds(
    const std::vector<Value>& params, const std::vector<Value>& installed, Context* ctx);
// Defined in primitives/ports.cpp: maps a current-*-port accessor primitive to
// its backing parameter object so parameterize can rebind it (R7RS 6.13.1).
Value port_parameter_for_accessor(const std::string& name, Context* ctx);
static Value cek_loop(const Value& expr, Environment* env, Context* ctx);

// ── isFalse ───────────────────────────────────────────────────────────────────

static inline bool is_false_val(const Value& v)
   {
   return is_boolean(v) && !as_boolean(v);
   }

// ── collect_cons_to_list ──────────────────────────────────────────────────────

static std::vector<Value> collect_cons_to_list(const Value& cell)
   {
   std::vector<Value> items;
   Value cur = cell;
   while (is_cons(cur))
      {
      items.push_back(car(cur));
      cur = cdr(cur);
      }
   return items;
   }

// ── collect_let_bindings ──────────────────────────────────────────────────────

static std::vector<std::pair<uint32_t, Value>> collect_let_bindings(const Value& bindings)
   {
   std::vector<std::pair<uint32_t, Value>> pairs;
   Value cur = bindings;
   while (is_cons(cur))
      {
      Value b = car(cur);
      pairs.push_back({as_symbol_id(car(b)), car(cdr(b))});
      cur = cdr(cur);
      }
   return pairs;
   }

// ── make_closure_from_lambda ──────────────────────────────────────────────────

static Value make_closure_from_lambda(const Value& lam, Environment* env)
   {
   Value params_sexpr = car(cdr(lam));
   Value body_cons = cdr(cdr(lam));
   std::vector<uint32_t> params;
   uint32_t rest_id = UINT32_MAX;
   if (is_symbol(params_sexpr))
      {
      rest_id = as_symbol_id(params_sexpr);
      }
   else if (is_cons(params_sexpr) || is_nil(params_sexpr))
      {
      Value cur = params_sexpr;
      while (is_cons(cur))
         {
         params.push_back(as_symbol_id(car(cur)));
         cur = cdr(cur);
         }
      if (is_symbol(cur))
         rest_id = as_symbol_id(cur);
      }
   std::string docstring;
   if (is_cons(body_cons) && is_cons(cdr(body_cons)) && is_string(car(body_cons)))
      {
      docstring = as_string(car(body_cons));
      body_cons = cdr(body_cons);
      }
   return make_closure(std::move(params), body_cons, env, rest_id, std::move(docstring));
   }

// ── make_case_closure_from_form ───────────────────────────────────────────────

static Value make_case_closure_from_form(const Value& cl_cons, Environment* env)
   {
   std::vector<CaseClosure::Clause> clauses;
   Value cur = cdr(cl_cons);
   while (is_cons(cur))
      {
      Value clause = car(cur);
      Value ps = car(clause);
      CaseClosure::Clause cl;
      cl.rest_name_id = UINT32_MAX;
      if (is_symbol(ps))
         {
         cl.rest_name_id = as_symbol_id(ps);
         }
      else if (is_cons(ps) || is_nil(ps))
         {
         Value p = ps;
         while (is_cons(p))
            {
            cl.params.push_back(as_symbol_id(car(p)));
            p = cdr(p);
            }
         if (is_symbol(p))
            cl.rest_name_id = as_symbol_id(p);
         }
      cl.body = cdr(clause);
      clauses.push_back(std::move(cl));
      cur = cdr(cur);
      }
   return make_case_closure(std::move(clauses), env, "");
   }

// ── beta_reduce_core ──────────────────────────────────────────────────────────

static BetaResult beta_reduce_core(const std::vector<uint32_t>& params, const Value& body,
                                   Environment* clo_env, uint32_t rest_id,
                                   const std::vector<Value>& arg_values, const Value* app_node)
   {
   int n_fixed = (int)params.size();
   int n_args = (int)arg_values.size();
   SourceInfo* src = app_node ? src_of(*app_node) : nullptr;
   if (rest_id == UINT32_MAX)
      {
      if (n_fixed != n_args)
         throw SchemeArityError(arity_mismatch_msg("", n_fixed, n_fixed, n_args), src);
      }
   else
      {
      if (n_args < n_fixed)
         throw SchemeArityError(arity_mismatch_msg("", n_fixed, -1, n_args), src);
      }
   Environment* new_env = gc_alloc_environment(clo_env);
   for (int i = 0; i < n_fixed; ++i)
      new_env->bind_id(params[i], arg_values[i]);
   if (rest_id != UINT32_MAX)
      {
      Value rest_val = NIL_VALUE;
      for (int i = n_args - 1; i >= n_fixed; --i)
         rest_val = alloc_cons(arg_values[i], rest_val);
      new_env->bind_id(rest_id, rest_val);
      }
   return {new_env, body};
   }

// ── apply_value ───────────────────────────────────────────────────────────────

static BetaResult apply_value(const Value& V, const std::vector<Value>& arg_values,
                              const Value* app_node)
   {
   if (is_case_closure(V))
      {
      const auto& clauses = as_case_closure_clauses(V);
      Environment* clo_env = as_case_closure_env(V);
      int n_args = (int)arg_values.size();
      for (const auto& c : clauses)
         {
         int n_fixed = (int)c.params.size();
         if (c.rest_name_id == UINT32_MAX)
            {
            if (n_fixed == n_args)
               return beta_reduce_core(c.params, c.body, clo_env, UINT32_MAX, arg_values, app_node);
            }
         else
            {
            if (n_args >= n_fixed)
               return beta_reduce_core(c.params, c.body, clo_env, c.rest_name_id, arg_values, app_node);
            }
         }
      SourceInfo* src = app_node ? src_of(*app_node) : nullptr;
      throw SchemeArityError(
          "case-lambda: no clause matches " + std::to_string(n_args) + " arguments", src);
      }
   if (!is_closure(V))
      {
      SourceInfo* src = app_node ? src_of(*app_node) : nullptr;
      throw SchemeTypeError("application of non-procedure: " + scheme_pretty_print(V), src);
      }
   return beta_reduce_core(as_closure_params(V), as_closure_body(V),
                           as_closure_env(V), as_closure_rest_name(V), arg_values, app_node);
   }

// ── primitive name predicates ─────────────────────────────────────────────────
// Special-primitive interception is dispatched on the integer kind tag set
// by make_primitive (AST PRIM_*), read once per application in the
// FRAME_CALL handler.  This replaced a ladder of ~15 per-call is_X_prim
// name comparisons (optimization #2).  is_parameterize_restore_prim
// remains: it tests a stored wind-frame value (not fn_value) at the
// %with-parameters tail-call check, mirroring Evaluator.py's inline
// name compare there.

static inline bool is_parameterize_restore_prim(const Value& V)
   {
   return is_native_closure(V) && as_native_closure_name(V) == "%parameterize-restore";
   }

// ── is_aux_keyword ────────────────────────────────────────────────────────────

static bool is_aux_keyword(const Value& sym, const std::string& name, Environment* env)
   {
   if (!is_symbol(sym) || as_symbol(sym) != name)
      return false;
   if (!env)
      return true;
   return !env->lookup_optional(name).has_value();
   }

// ── classify_cond_clause ──────────────────────────────────────────────────────

static CondClause classify_cond_clause(const Value& clause, Environment* env)
   {
   Value head = car(clause);
   if (is_aux_keyword(head, "else", env))
      {
      CondClause cc;
      cc.kind = CondClause::Else;
      cc.body_cons = cdr(clause);
      return cc;
      }
   if (is_nil(cdr(clause)))
      {
      CondClause cc;
      cc.kind = CondClause::TestOnly;
      cc.test = head;
      return cc;
      }
   Value tail = cdr(clause);
   if (is_cons(tail) && is_aux_keyword(car(tail), "=>", env) && is_cons(cdr(tail)) && is_nil(cdr(cdr(tail))))
      {
      CondClause cc;
      cc.kind = CondClause::Arrow;
      cc.test = head;
      cc.proc_expr = car(cdr(tail));
      return cc;
      }
   CondClause cc;
   cc.kind = CondClause::Body;
   cc.test = head;
   cc.body_cons = cdr(clause);
   return cc;
   }

// ── shadow helpers ────────────────────────────────────────────────────────────

static std::string shadow_label_of(const Value* app_node)
   {
   if (app_node && is_cons(*app_node) && is_symbol(car(*app_node)))
      return as_symbol(car(*app_node));
   return "#<procedure>";
   }

static void shadow_push(Context* ctx, KStack& K, const Value* app_node)
   {
   auto& ss = ctx->shadow_stack;
   std::string label = shadow_label_of(app_node);
   SourceInfo* src = app_node ? src_of(*app_node) : nullptr;
   if (!K.empty() && K.back().tag == FRAME_SHADOW_POP)
      {
      if (!ss.empty())
         {
         ShadowEntry& top = ss.back();
         if (top.label == label && top.src == src)
            {
            ++top.count;
            return;
            }
         ss.back() = {label, src, 1};
         }
      return;
      }
   if ((int)ss.size() >= SHADOW_DEPTH_LIMIT)
      return;
   if (!ss.empty())
      {
      ShadowEntry& top = ss.back();
      if (top.label == label && top.src == src)
         {
         ++top.count;
         Frame f;
         f.tag = FRAME_SHADOW_POP;
         K.push_back(std::move(f));
         return;
         }
      }
   ss.push_back({label, src, 1});
   Frame f;
   f.tag = FRAME_SHADOW_POP;
   K.push_back(std::move(f));
   }

// ── continuation_value ────────────────────────────────────────────────────────

static Value continuation_value(const std::vector<Value>& args)
   {
   if (args.empty())
      return VOID_VALUE;
   if (args.size() == 1)
      return args[0];
   return make_multi_values(std::vector<Value>(args));
   }


// ── apply_parameter_if ────────────────────────────────────────────────────────

static std::optional<Value> apply_parameter_if(const Value& V, int n_args, const Value* app_node)
   {
   if (!is_parameter(V))
      return std::nullopt;
   if (n_args != 0)
      {
      SourceInfo* src = app_node ? src_of(*app_node) : nullptr;
      throw SchemeArityError(arity_mismatch_msg("parameter", 0, 0, n_args), src);
      }
   return as_parameter_value(V);
   }

// ── restore helpers ───────────────────────────────────────────────────────────

static void restore_handler_stack(Context* ctx, const std::vector<Value>& snapshot)
   {
   ctx->handler_stack.assign(snapshot.begin(), snapshot.end());
   }

static void restore_shadow_stack(Context* ctx, const std::vector<Value>& snapshot)
   {
   ctx->shadow_stack.clear();
   for (const Value& v : snapshot)
      {
      ShadowEntry e;
      e.label = as_symbol(car(v));
      e.count = (int)as_integer(cdr(v));
      e.src = nullptr;
      ctx->shadow_stack.push_back(e);
      }
   }

// ── value identity pointer (for the wind common-prefix diff) ──────────────────

static const void* val_id(const Value& v)
   {
   GcHeader* h = gc_value_header(v);
   if (h)
      return h;
   if (is_primitive(v))
      return static_cast<const void*>(&as_primitive_fn(v));
   return nullptr;
   }

// ── apply_scheme_proc ─────────────────────────────────────────────────────────

Value apply_scheme_proc(const Value& fn, std::vector<Value> args,
                        Context* ctx, Environment* env, const Value* app_node)
   {
   if (is_primitive(fn))
      return as_primitive_fn(fn)(ctx, env, args, app_node);
   if (is_native_closure(fn))
      return as_native_closure_fn(fn)(ctx, env, args,
                                      as_native_closure_captures(fn), app_node);
   if (is_closure(fn) || is_case_closure(fn))
      {
      BetaResult r = apply_value(fn, args, app_node);
      const Value& body = r.body;
      if (is_cons(body) && is_nil(cdr(body)))
         return cek_eval(car(body), r.new_env, ctx);
      Value bf = alloc_cons(make_symbol("begin"), body);
      return cek_eval(bf, r.new_env, ctx);
      }
   SourceInfo* src = app_node ? src_of(*app_node) : nullptr;
   // Record accessors / mutators are first-class procedures too (R7RS 5.5), so
   // map / for-each / apply / call-with-values must apply them, matching the
   // eval loop's application dispatch.
   if (is_record_accessor(fn))
      {
      if (args.size() != 1)
         throw SchemeArityError(
             arity_mismatch_msg(as_record_accessor_name(fn), 1, 1, (int)args.size()), src);
      RecordType* rt = as_record_accessor_type(fn);
      if (!is_record(args[0]) || as_record_type(args[0]) != rt)
         throw SchemeTypeError(
             as_record_accessor_name(fn) + ": argument is not a " + rt->name, src);
      return as_record_fields_const(args[0])[as_record_accessor_index(fn)];
      }
   if (is_record_mutator(fn))
      {
      if (args.size() != 2)
         throw SchemeArityError(
             arity_mismatch_msg(as_record_mutator_name(fn), 2, 2, (int)args.size()), src);
      RecordType* rt = as_record_mutator_type(fn);
      if (!is_record(args[0]) || as_record_type(args[0]) != rt)
         throw SchemeTypeError(
             as_record_mutator_name(fn) + ": first argument is not a " + rt->name, src);
      as_record_fields(args[0])[as_record_mutator_index(fn)] = args[1];
      return VOID_VALUE;
      }
   throw SchemeTypeError("expected a procedure", src);
   }

// ── unwind_winds_on_error ─────────────────────────────────────────────────────

static void unwind_winds_on_error(Context* ctx, size_t target_depth)
   {
   auto& ws = ctx->wind_stack;
   while (ws.size() > target_depth)
      {
      Value after = ws.back().after;
      ws.pop_back();
      try
         {
         std::vector<Value> ea;
         apply_scheme_proc(after, ea, ctx, nullptr, nullptr);
         }
      catch (...)
         {
         }
      }
   }

// ── make_wind_step_frame (compute_wind_ops) ───────────────────────────────────

// Build the FRAME_WIND_STEP driver for a continuation jump.  Computes the ordered
// wind operations that transform ctx->wind_stack into `target` (the continuation's
// wind snapshot) WITHOUT mutating the stack or running any thunk: exits first
// (innermost-first), then enters (outermost-first).  FRAME_WIND_STEP performs the
// pops/pushes and runs each thunk on the K stack, so a continuation jump across
// dynamic-wind / parameterize runs entirely on the one loop (no re-entrant
// cek_eval / no ContinuationEscape).  Mirrors pyScheme's _compute_wind_ops + the
// ('frame', (FRAME_WIND_STEP, ...)) descriptor.
//   ids[i] = 0 -> exit  (pop wind_stack, run list1[i] = the popped after)
//   ids[i] = 1 -> enter (push {list1[i], list2[i]}, run list1[i] = the before)
//   v1 = cont, v2 = cont_value, depth = current op index
static Frame make_wind_step_frame(Context* ctx, const std::vector<WindFrame>& target,
                                  const Value& cont, const Value& value)
   {
   auto& ws = ctx->wind_stack;
   size_t common = 0;
   while (common < ws.size() && common < target.size())
      {
      if (val_id(ws[common].before) != val_id(target[common].before) ||
          val_id(ws[common].after) != val_id(target[common].after))
         break;
      ++common;
      }
   Frame f;
   f.tag = FRAME_WIND_STEP;
   f.v1 = cont;
   f.v2 = value;
   f.depth = 0;
   for (size_t j = ws.size(); j > common; --j) // exits, innermost-first
      {
      f.ids.push_back(0);
      f.list1.push_back(ws[j - 1].after);
      f.list2.push_back(NIL_VALUE);
      }
   for (size_t i = common; i < target.size(); ++i) // enters, outermost-first
      {
      f.ids.push_back(1);
      f.list1.push_back(target[i].before);
      f.list2.push_back(target[i].after);
      }
   return f;
   }

// ── enter_proc ────────────────────────────────────────────────────────────────

// If fn_value is a higher-order primitive whose per-element calls are driven on
// the K stack -- map/for-each/filter (FRAME_HOF_STEP), the vector/string variants
// (FRAME_HOF_STEP_IDX), or the 3-arg member/assoc forms (FRAME_SEARCH_STEP) --
// build and return its driver frame; otherwise std::nullopt.  Shared by the
// FRAME_CALL terminal dispatch and enter_proc so one of these primitives reached
// as a callback (e.g. the per-element proc of an outer map, as in (map filter ...))
// is driven on frames too, rather than re-entering cek_eval through its _prim_*
// fallback.  Single source of truth -- mirrors pyScheme's _build_hof_frame.
static std::optional<Frame> build_hof_frame(const Value& fn_value, int kind,
                                            const std::vector<Value>& collected,
                                            const Value* app_node)
   {
   SourceInfo* src = app_node ? src_of(*app_node) : nullptr;
   Value app = app_node ? *app_node : NIL_VALUE;
   if (kind == PRIM_MAP || kind == PRIM_FOR_EACH || kind == PRIM_FILTER)
      {
      const std::string& nm = as_primitive_name(fn_value);
      if (kind == PRIM_FILTER)
         {
         if (collected.size() != 2)
            throw SchemeArityError(
                arity_mismatch_msg(nm, 2, 2, (int)collected.size()), src);
         }
      else if (collected.size() < 2)
         throw SchemeArityError(
             arity_mismatch_msg(nm, 2, -1, (int)collected.size()), src);
      Frame hf;
      hf.tag = FRAME_HOF_STEP;
      hf.depth = kind;           // mode
      hf.uid = 0;                // started = false
      hf.v1 = collected[0];      // proc
      hf.v2 = NIL_VALUE;         // acc (reversed cons)
      hf.list1.assign(collected.begin() + 1, collected.end()); // cursors
      hf.list2 = {app, NIL_VALUE};  // {app_node, pending}
      return hf;
      }
   if (kind == PRIM_VECTOR_MAP || kind == PRIM_VECTOR_FOR_EACH ||
       kind == PRIM_STRING_MAP || kind == PRIM_STRING_FOR_EACH)
      {
      const std::string& nm = as_primitive_name(fn_value);
      if (collected.size() < 2)
         throw SchemeArityError(
             arity_mismatch_msg(nm, 2, -1, (int)collected.size()), src);
      bool is_vec = (kind == PRIM_VECTOR_MAP || kind == PRIM_VECTOR_FOR_EACH);
      for (size_t j = 1; j < collected.size(); ++j)
         {
         if (is_vec)
            {
            if (!is_vector(collected[j]))
               throw SchemeTypeError(
                   nm + ": argument " + std::to_string(j + 1) + " must be a vector", src);
            }
         else if (!is_string(collected[j]))
            throw SchemeTypeError(
                nm + ": argument " + std::to_string(j + 1) + " must be a string", src);
         }
      Frame hf;
      hf.tag = FRAME_HOF_STEP_IDX;
      hf.depth = kind;           // mode
      hf.uid = 0;                // started = false
      hf.v1 = collected[0];      // proc
      hf.v2 = NIL_VALUE;         // acc (reversed cons)
      hf.list1.assign(collected.begin() + 1, collected.end()); // seqs
      hf.list2 = {app};
      hf.ids.assign(collected.size() - 1, 0u); // positions, all 0
      return hf;
      }
   if ((kind == PRIM_MEMBER || kind == PRIM_ASSOC) && collected.size() == 3)
      {
      Frame hf;
      hf.tag = FRAME_SEARCH_STEP;
      hf.depth = kind;              // mode
      hf.uid = 0;                   // started = false
      hf.v1 = collected[2];         // proc (comparator)
      hf.v2 = collected[0];         // target
      hf.list1 = {collected[1]};    // cursor (list head)
      hf.list2 = {app};
      return hf;
      }
   return std::nullopt;
   }


static EnterResult enter_proc(const Value& fn_value, std::vector<Value>& args,
                              Context* ctx, Environment* saved_env, const Value* app_node)
   {
   if (is_continuation(fn_value))
      {
      // Drive the wind walk on the K stack (FRAME_WIND_STEP), which then installs
      // the continuation.  Delivered via the IsFrame descriptor every enter_proc
      // caller already handles, so the wind before/after thunks run without
      // re-entering cek_eval.  (All evaluation now runs on one loop, so a
      // continuation is always installed in place -- no escape across native
      // frames is ever needed.)
      EnterResult r;
      r.kind = EnterResult::IsFrame;
      r.frame = make_wind_step_frame(ctx, as_continuation_wind(fn_value),
                                     fn_value, continuation_value(args));
      return r;
      }
   auto pv = apply_parameter_if(fn_value, (int)args.size(), app_node);
   if (pv.has_value())
      {
      EnterResult r;
      r.kind = EnterResult::IsValue;
      r.v = *pv;
      return r;
      }
   if (is_native_closure(fn_value))
      {
      Value v = as_native_closure_fn(fn_value)(
          ctx, saved_env, args, as_native_closure_captures(fn_value), app_node);
      EnterResult r;
      r.kind = EnterResult::IsValue;
      r.v = v;
      return r;
      }
   if (is_primitive(fn_value))
      {
      int pkind = as_primitive_kind(fn_value);
      auto hof = build_hof_frame(fn_value, pkind, args, app_node);
      if (hof.has_value())
         {
         EnterResult r;
         r.kind = EnterResult::IsFrame;
         r.frame = std::move(*hof);
         return r;
         }
      if (pkind == PRIM_LOAD)
         {
         // load reached as a callback (e.g. (for-each load files)): read + parse
         // and drive its forms on the K stack, like the FRAME_CALL interception,
         // rather than re-entering cek_eval via _prim_load.
         LoadSetup ls = load_setup(args, saved_env, app_node);
         Frame ef;
         ef.tag = FRAME_EVAL_FORMS;
         ef.list1 = std::move(ls.forms);
         ef.env = ls.eval_env;
         ef.depth = 0;
         EnterResult r;
         r.kind = EnterResult::IsFrame;
         r.frame = std::move(ef);
         return r;
         }
      Value v = as_primitive_fn(fn_value)(ctx, saved_env, args, app_node);
      EnterResult r;
      r.kind = EnterResult::IsValue;
      r.v = v;
      return r;
      }
   if (is_closure(fn_value) || is_case_closure(fn_value))
      {
      BetaResult br = apply_value(fn_value, args, app_node);
      EnterResult r;
      r.kind = EnterResult::IsEnter;
      r.v = car(br.body);
      r.new_env = br.new_env;
      r.seq = is_cons(cdr(br.body)) ? cdr(br.body) : NIL_VALUE;
      return r;
      }
   SourceInfo* src = app_node ? src_of(*app_node) : nullptr;
   // Record accessors / mutators are first-class procedures (R7RS 5.5), so the
   // FRAME_HOF_STEP / FRAME_CWV_CONSUMER paths that tail-call through here must
   // apply them too, matching apply_scheme_proc and the FRAME_CALL terminal dispatch.
   if (is_record_accessor(fn_value))
      {
      if (args.size() != 1)
         throw SchemeArityError(
             arity_mismatch_msg(as_record_accessor_name(fn_value), 1, 1, (int)args.size()), src);
      RecordType* rt = as_record_accessor_type(fn_value);
      if (!is_record(args[0]) || as_record_type(args[0]) != rt)
         throw SchemeTypeError(
             as_record_accessor_name(fn_value) + ": argument is not a " + rt->name, src);
      EnterResult r;
      r.kind = EnterResult::IsValue;
      r.v = as_record_fields_const(args[0])[as_record_accessor_index(fn_value)];
      return r;
      }
   if (is_record_mutator(fn_value))
      {
      if (args.size() != 2)
         throw SchemeArityError(
             arity_mismatch_msg(as_record_mutator_name(fn_value), 2, 2, (int)args.size()), src);
      RecordType* rt = as_record_mutator_type(fn_value);
      if (!is_record(args[0]) || as_record_type(args[0]) != rt)
         throw SchemeTypeError(
             as_record_mutator_name(fn_value) + ": first argument is not a " + rt->name, src);
      as_record_fields(args[0])[as_record_mutator_index(fn_value)] = args[1];
      EnterResult r;
      r.kind = EnterResult::IsValue;
      r.v = VOID_VALUE;
      return r;
      }
   throw SchemeTypeError("expected a procedure", src);
   }

// ── apply_enter_result ────────────────────────────────────────────────────────
// Outcome of applying an enter_proc descriptor to the CEK registers.
enum class EnterAction { DoApply, DoEval };

// Apply an enter_proc descriptor to the CEK registers and report what the APPLY
// loop should do next.  K is mutated in place when a frame must be pushed; V is
// set for a produced value; C/E are set when a closure body is entered.
//   DoApply -- a value was produced (or a frame-driven callback was pushed):
//              stay in / resume the APPLY phase (caller: continue, or set
//              skip_eval at the handler-dispatch site)
//   DoEval  -- a closure body was entered: evaluate C in E (caller: break)
// enter_proc never yields IsCont since the single-loop rewrite (a continuation
// arrives as a FRAME_WIND_STEP IsFrame), so that case is gone.  Mirrors
// pyScheme's _apply_enter_result.
static EnterAction apply_enter_result(EnterResult& result, KStack& K,
                                      Value& V, Value& C, Environment*& E)
   {
   if (result.kind == EnterResult::IsValue)
      {
      V = result.v;
      return EnterAction::DoApply;
      }
   if (result.kind == EnterResult::IsFrame)
      {
      K.push_back(std::move(result.frame));
      return EnterAction::DoApply;
      }
   // IsEnter: a closure body to evaluate; push FRAME_SEQ for a multi-form body.
   C = result.v;
   E = result.new_env;
   if (is_cons(result.seq))
      {
      Frame sf;
      sf.tag = FRAME_SEQ;
      sf.v1 = result.seq;
      sf.env = result.new_env;
      K.push_back(std::move(sf));
      }
   return EnterAction::DoEval;
   }

// ── unpack_apply_args ─────────────────────────────────────────────────────────

static std::pair<Value, std::vector<Value>> unpack_apply_args(
    const std::vector<Value>& collected, const Value* app_node)
   {
   if ((int)collected.size() < 2)
      {
      SourceInfo* src = app_node ? src_of(*app_node) : nullptr;
      throw SchemeArityError(arity_mismatch_msg("apply", 2, -1, (int)collected.size()), src);
      }
   Value proc = collected[0];
   std::vector<Value> flat;
   for (size_t i = 1; i + 1 < collected.size(); ++i)
      flat.push_back(collected[i]);
   Value last = collected.back();
   Value cur = last;
   while (is_cons(cur))
      {
      flat.push_back(car(cur));
      cur = cdr(cur);
      }
   if (!is_nil(cur))
      {
      SourceInfo* src = app_node ? src_of(*app_node) : nullptr;
      throw SchemeTypeError("apply: last argument must be a proper list", src);
      }
   return {proc, std::move(flat)};
   }

// ── is_multi_values_ok_frame ──────────────────────────────────────────────────

static bool is_mv_ok(int ftag)
   {
   switch (ftag)
      {
   case FRAME_CWV_CONSUMER:
   case FRAME_SEQ:
   case FRAME_DYNAMIC_WIND_AFTER:
   case FRAME_POP_HANDLER:
   case FRAME_REINSTALL_HANDLER:
   case FRAME_NONCONTIN_RETURN:
   case FRAME_SHADOW_POP:
   case FRAME_TRACE_EXIT:
   case FRAME_GUARD:
   case FRAME_HOF_STEP:
   case FRAME_HOF_STEP_IDX:
   case FRAME_SEARCH_STEP:
   // FRAME_RESTORE_VALUE discards the incoming V (a wind after-thunk's result)
   // and reinstates a saved value, so a multi-valued after result is harmless.
   case FRAME_RESTORE_VALUE:
   // FRAME_DYNAMIC_WIND_BEFORE_DONE discards the before-thunk's result before
   // tail-calling the body, so a multi-valued before result is harmless.
   case FRAME_DYNAMIC_WIND_BEFORE_DONE:
   // FRAME_PARAMETERIZE_STEP collects each converter's result as an installed
   // parameter value; tolerate a multi-valued converter result as the old
   // synchronous converter application did.
   case FRAME_PARAMETERIZE_STEP:
   // FRAME_WIND_STEP discards each wind thunk's result and finally installs the
   // continuation's value (which may itself be multiple values).
   case FRAME_WIND_STEP:
   // FRAME_ERROR_UNWIND discards each unwind after-thunk's result before
   // re-raising the original condition.
   case FRAME_ERROR_UNWIND:
   // The load / library-loading drivers discard each form's / step's result
   // and finally yield VOID, so an intermediate multi-valued result is harmless.
   case FRAME_EVAL_FORMS:
   case FRAME_LIB_FINALIZE:
   case FRAME_IMPORT_STEP:
   case FRAME_ENSURE_LOADED:
      return true;
   default:
      return false;
      }
   }

// ── parameterize winds ────────────────────────────────────────────────────────

// Shared body for the %parameterize-install / %parameterize-restore native
// closures: captures = [params... , values...] (two equal halves); set each
// parameter to its paired value.  Stateless (a NativeFn function pointer); the
// captured Values are GC-traced/forwarded by GcType::NativeClosure, unlike the
// opaque std::function captures of the old make_primitive winds.
static Value nc_parameterize_set(Context*, Environment*, std::vector<Value>&,
                                 const std::vector<Value>& cap, const Value*)
   {
   size_t n = cap.size() / 2;
   for (size_t j = 0; j < n; ++j)
      set_parameter_value(const_cast<Value&>(cap[j]), cap[n + j]);
   return VOID_VALUE;
   }

// Phase 1 of parameterize setup: walk the parameter / value lists, resolve
// current-port accessor primitives to their backing parameters, and validate.
// Returns (params, new_vals_raw).  Pure -- no converter application, no install --
// so the converters can run on the K stack (FRAME_PARAMETERIZE_STEP) instead of
// re-entering cek_eval.  Mirrors pyScheme's _resolve_parameterize_params.
static std::pair<std::vector<Value>, std::vector<Value>> resolve_parameterize_params(
    const Value& params_list, const Value& values_list, Context* ctx, const Value* app_node)
   {
   std::vector<Value> params;
   Value cur = params_list;
   while (is_cons(cur))
      {
      Value p = car(cur);
      if (!is_parameter(p) && is_primitive(p))
         {
         // R7RS 6.13.1: current-output-port / current-input-port /
         // current-error-port ARE parameter objects.  They are exposed as
         // accessor primitives; map the accessor to its backing parameter
         // so parameterize can rebind it.
         Value backing = port_parameter_for_accessor(as_primitive_name(p), ctx);
         if (is_parameter(backing))
            p = backing;
         }
      if (!is_parameter(p))
         {
         SourceInfo* s = app_node ? src_of(*app_node) : nullptr;
         throw SchemeTypeError("%with-parameters: non-parameter in parameterize binding", s);
         }
      params.push_back(p);
      cur = cdr(cur);
      }
   if (!is_nil(cur))
      {
      SourceInfo* s = app_node ? src_of(*app_node) : nullptr;
      throw SchemeTypeError("%with-parameters: parameter list must be proper", s);
      }

   std::vector<Value> new_vals_raw;
   cur = values_list;
   while (is_cons(cur))
      {
      new_vals_raw.push_back(car(cur));
      cur = cdr(cur);
      }
   if (!is_nil(cur))
      {
      SourceInfo* s = app_node ? src_of(*app_node) : nullptr;
      throw SchemeTypeError("%with-parameters: value list must be proper", s);
      }
   if (params.size() != new_vals_raw.size())
      {
      SourceInfo* s = app_node ? src_of(*app_node) : nullptr;
      throw SchemeTypeError("%with-parameters: parameter / value count mismatch", s);
      }
   return {std::move(params), std::move(new_vals_raw)};
   }

// Phase 2 of parameterize setup, run once FRAME_PARAMETERIZE_STEP has applied
// every converter and collected the `installed` values: save the current values,
// install the new ones so the thunk sees them, and return a pair of GC-managed
// native closures (install, restore) that FRAME_WIND_STEP and
// FRAME_DYNAMIC_WIND_AFTER / FRAME_ERROR_UNWIND invoke as Scheme procedures.
// Saving after the converters (not before) matches the old ordering.  Mirrors
// pyScheme's _finalize_parameterize_winds.
static std::pair<Value, Value> finalize_parameterize_winds(
    const std::vector<Value>& params, const std::vector<Value>& installed, Context* ctx)
   {
   (void)ctx;
   size_t n = params.size();
   std::vector<Value> saved_vals;
   for (size_t i = 0; i < n; ++i)
      saved_vals.push_back(as_parameter_value(params[i]));
   // Install new values now so the thunk sees them.
   for (size_t i = 0; i < n; ++i)
      set_parameter_value(const_cast<Value&>(params[i]), installed[i]);
   // captures = [params... , values...] for the shared nc_parameterize_set body.
   std::vector<Value> inst_cap;
   inst_cap.reserve(2 * n);
   for (size_t i = 0; i < n; ++i) inst_cap.push_back(params[i]);
   for (size_t i = 0; i < n; ++i) inst_cap.push_back(installed[i]);
   std::vector<Value> rest_cap;
   rest_cap.reserve(2 * n);
   for (size_t i = 0; i < n; ++i) rest_cap.push_back(params[i]);
   for (size_t i = 0; i < n; ++i) rest_cap.push_back(saved_vals[i]);
   return {make_native_closure("%parameterize-install", nc_parameterize_set, std::move(inst_cap)),
           make_native_closure("%parameterize-restore", nc_parameterize_set, std::move(rest_cap))};
   }

// ── Library search path (parameter-backed) ──────────────────────────────────
// Port of Evaluator.py: the .sld search path lives in a current-library-path
// parameter so a program can read it, rebind it with parameterize, or replace
// it persistently with set-library-path!.  _library_load_path reads the
// parameter's current value; until an interpreter binds it (Evaluator self-test,
// etc.) it falls back to '.' + SCHEME_LIBRARY_PATH.
//
// C port: pyScheme keeps the parameter in a module-level [None] holder; here the
// Value is a file-static registered as a GC root (gc_root_push) so the moving GC
// keeps the slot updated.  (pyScheme also marks the config strings immutable;
// omitted here as there is no public string-immutable setter and it is cosmetic.)
static Value g_library_path_param;
static bool g_library_path_param_set = false;

static void set_library_path_param(Value p)
   {
   if (!g_library_path_param_set)
      gc_root_push(&g_library_path_param);
   g_library_path_param = p;
   g_library_path_param_set = true;
   }

// The SCHEME_LIBRARY_PATH portion of the default path: env var split on os
// pathsep (';' on Windows, ':' on Unix), empties dropped.
static std::vector<std::string> _env_library_path_parts()
   {
   std::vector<std::string> parts;
   const char* path_var = getenv("SCHEME_LIBRARY_PATH");
   if (path_var == nullptr)
      return parts;
   std::string pv(path_var);
#ifdef _WIN32
   const char sep = ';';
#else
   const char sep = ':';
#endif
   size_t start = 0;
   while (true)
      {
      size_t pos = pv.find(sep, start);
      std::string part = pv.substr(start,
                                   pos == std::string::npos ? pos : pos - start);
      if (!part.empty())
         parts.push_back(part);
      if (pos == std::string::npos)
         break;
      start = pos + 1;
      }
   return parts;
   }

// Default search path when no current-library-path parameter is in effect:
// current directory first, then SCHEME_LIBRARY_PATH entries.
static std::vector<std::string> _default_library_path()
   {
   std::vector<std::string> parts = {"."};
   for (const std::string& p : _env_library_path_parts())
      parts.push_back(p);
   return parts;
   }

// Initial library search path: current dir, then the CLI -L/-I paths (in
// command-line order), then SCHEME_LIBRARY_PATH.
static std::vector<std::string> build_library_path_list(
    const std::vector<std::string>& cli_paths)
   {
   std::vector<std::string> parts = {"."};
   for (const std::string& p : cli_paths)
      if (!p.empty())
         parts.push_back(p);
   for (const std::string& p : _env_library_path_parts())
      parts.push_back(p);
   return parts;
   }

// Build a Scheme proper list of strings from a native vector of directories.
static Value _make_scheme_string_list(const std::vector<std::string>& paths)
   {
   Value result = NIL_VALUE;
   for (size_t i = paths.size(); i-- > 0;)
      result = alloc_cons(make_string(paths[i]), result);
   return result;
   }

// Validate that val is a proper list of strings and return a fresh Scheme list
// of the same strings.  Used as the current-library-path parameter's converter
// (so parameterize is validated) and by set-library-path!.  Throws on a
// non-list or a non-string element.
Value normalize_library_path_value(const Value& val, const Value* app_node)
   {
   std::vector<std::string> paths;
   Value cur = val;
   while (is_cons(cur))
      {
      Value elt = car(cur);
      if (!is_string(elt))
         throw SchemeTypeError("library path must be a list of strings",
                               app_node ? src_of(*app_node) : nullptr);
      paths.push_back(as_string(elt));
      cur = cdr(cur);
      }
   if (!is_nil(cur))
      throw SchemeTypeError("library path must be a proper list",
                            app_node ? src_of(*app_node) : nullptr);
   return _make_scheme_string_list(paths);
   }

static Value _library_path_converter_fn(Context*, Environment*,
                                        std::vector<Value>& args,
                                        const Value* app_node)
   {
   return normalize_library_path_value(args[0], app_node);
   }

// Create the current-library-path parameter from '.' + CLI paths + env, install
// it as the loader's source of truth, and return it for binding into the env.
Value make_library_path_param(const std::vector<std::string>& cli_paths)
   {
   Value init_val = _make_scheme_string_list(build_library_path_list(cli_paths));
   Value converter = make_primitive("%library-path-converter",
                                    _library_path_converter_fn);
   Value param = make_parameter(init_val, converter);
   set_library_path_param(param);
   return param;
   }

// True once an interpreter has installed the current-library-path parameter.
bool library_path_param_is_set()
   {
   return g_library_path_param_set;
   }

// Persistently replace the parameter's value (the engine behind
// set-library-path!).  Mirrors pyScheme reaching _library_path_param_ref[0].
void library_path_param_assign(Value normalized)
   {
   if (g_library_path_param_set)
      set_parameter_value(g_library_path_param, normalized);
   }

// ── _library_load_path ──────────────────────────────────────────────────────
// Port of Evaluator.py _library_load_path.  Returns the .sld search path as a
// vector of directory strings: the current-library-path parameter's current
// value when set (so parameterize / set-library-path! take effect), else the
// default ('.' + SCHEME_LIBRARY_PATH).
static std::vector<std::string> _library_load_path()
   {
   if (!g_library_path_param_set)
      return _default_library_path();
   std::vector<std::string> paths;
   Value cur = as_parameter_value(g_library_path_param);
   while (is_cons(cur))
      {
      Value elt = car(cur);
      if (is_string(elt))
         paths.push_back(as_string(elt));
      cur = cdr(cur);
      }
   return paths;
   }

// ── _process_one_lib_decl ─────────────────────────────────────────────────────
// Port of Evaluator.py _process_one_lib_decl.  Collects export names, import-set
// sexprs, and begin / unknown-decl forms (unexpanded) -- all in declaration
// order.  Nothing is resolved or evaluated here: the imports are bound (loading
// library files if needed) and the forms evaluated later on the main K stack
// (FRAME_IMPORT_STEP / FRAME_EVAL_FORMS), so no re-entrant cek_eval.
// Forward declaration needed because cond-expand recurses into itself.

static void _process_one_lib_decl(
    const Value& decl, Environment* lib_env,
    std::vector<std::pair<std::string, std::string>>& export_names,
    std::vector<Value>& eval_forms,
    std::vector<Value>& import_sets,
    Context* ctx);

static void _process_one_lib_decl(
    const Value& decl, Environment* lib_env,
    std::vector<std::pair<std::string, std::string>>& export_names,
    std::vector<Value>& eval_forms,
    std::vector<Value>& import_sets,
    Context* ctx)
   {
   if (!is_cons(decl) || !is_symbol(car(decl)))
      throw SchemeSyntaxError(
          "define-library: declaration must be a list starting with a symbol",
          src_of(decl));
   std::string dsym = as_symbol(car(decl));
   Value dbody = cdr(decl);

   if (dsym == "import")
      {
      Value sets = dbody;
      while (is_cons(sets))
         {
         import_sets.push_back(car(sets));
         sets = cdr(sets);
         }
      return;
      }

   if (dsym == "export")
      {
      Value specs = dbody;
      while (is_cons(specs))
         {
         Value spec = car(specs);
         specs = cdr(specs);
         if (is_symbol(spec))
            {
            std::string nm = as_symbol(spec);
            export_names.push_back({nm, nm});
            }
         else if (is_cons(spec) && is_symbol(car(spec)) && as_symbol(car(spec)) == "rename")
            {
            Value r = cdr(spec);
            if (!is_cons(r) || !is_symbol(car(r)) || !is_cons(cdr(r)) || !is_symbol(car(cdr(r))))
               throw SchemeSyntaxError(
                   "define-library: malformed export rename", src_of(spec));
            export_names.push_back(
                {as_symbol(car(r)), as_symbol(car(cdr(r)))});
            }
         else
            {
            throw SchemeSyntaxError(
                "define-library: malformed export spec", src_of(spec));
            }
         }
      return;
      }

   if (dsym == "begin")
      {
      Value forms = dbody;
      while (is_cons(forms))
         {
         eval_forms.push_back(car(forms));
         forms = cdr(forms);
         }
      return;
      }

   if (dsym == "include-library-declarations")
      {
      std::string base_dir = include_base_dir(src_of(decl));
      Value paths = dbody;
      while (is_cons(paths))
         {
         Value path_val = car(paths);
         paths = cdr(paths);
         if (!is_string(path_val))
            throw SchemeSyntaxError(
                "include-library-declarations: filename must be a string",
                src_of(path_val));
         std::string requested = as_string(path_val);
         std::string resolved = base_dir.empty()
                                    ? requested
                                    : (fs::path(base_dir) / requested).string();
         std::ifstream f(resolved);
         if (!f.is_open())
            throw SchemeSyntaxError(
                "include-library-declarations: file not found: " + resolved,
                src_of(decl));
         std::string source((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
         f.close();
         std::vector<Value> inner_forms = scheme_parse(source, resolved);
         GcRootVec inner_root(inner_forms); // keep pending forms alive across GC
         for (const Value& inner : inner_forms)
            _process_one_lib_decl(inner, lib_env, export_names,
                                  eval_forms, import_sets, ctx);
         }
      return;
      }

   if (dsym == "cond-expand")
      {
      Value cur_clause = dbody;
      while (is_cons(cur_clause))
         {
         Value clause = cur_clause;
         cur_clause = cdr(cur_clause);
         if (!is_cons(car(clause)))
            throw SchemeSyntaxError(
                "cond-expand: clause must be a list", src_of(car(clause)));
         Value req = car(car(clause));
         Value body = cdr(car(clause));
         if (feature_req_matches(req))
            {
            Value cur_inner = body;
            while (is_cons(cur_inner))
               {
               _process_one_lib_decl(
                   car(cur_inner), lib_env, export_names,
                   eval_forms, import_sets, ctx);
               cur_inner = cdr(cur_inner);
               }
            return;
            }
         }
      // No clause matched: silently produce no declarations (R7RS).
      return;
      }

   // Unknown declaration keyword: collect for evaluation in lib_env on the main
   // loop (FRAME_EVAL_FORMS).  Covers stray (define ...) forms; the later expand
   // routes define-syntax through the active per-library macro scope (the
   // FRAME_EVAL_FORMS phase runs while the runtime env is lib_env).
   eval_forms.push_back(decl);
   }

// ── define-library ────────────────────────────────────────────────────────────
// Port of Evaluator.py define_library_setup / _finalize_define_library /
// _make_runtime_env_setter.

// Native runtime-env setter: capture[0] is an EnvBox; set the active per-library
// macro scope (for define-syntax) to it.  Used as a define-library wind's
// before/after so the scope is established on entry and restored on exit ON THE
// MAIN LOOP -- across normal return, error unwind, and continuation re-entry
// (replacing the old try/catch restore).
static Value nc_set_runtime_env(Context*, Environment*, std::vector<Value>&,
                                const std::vector<Value>& cap, const Value*)
   {
   set_runtime_env(as_environment(cap[0]));
   return VOID_VALUE;
   }

struct DefineLibrarySetup
   {
   Environment* lib_env = nullptr;
   std::vector<Value> eval_forms;
   std::vector<std::pair<std::string, std::string>> export_names;
   std::string key;
   std::vector<Value> import_sets;
   };

// Pre-pass for (define-library <name> <decl>...): validate, create the library
// env, and process the declarations IN ORDER -- collecting export names, import
// sets, and the begin / unknown-decl forms (unexpanded) for evaluation on the
// main K stack.  Evaluates no form and does NOT swap the runtime env (no
// define-syntax runs here); the evaluator swaps it around the FRAME_EVAL_FORMS
// phase and registers the library via FRAME_LIB_FINALIZE.
static DefineLibrarySetup define_library_setup(const Value& C, Context* ctx)
   {
   if (!is_cons(cdr(C)))
      throw SchemeSyntaxError("define-library: missing library name", src_of(C));
   Value name_sexpr = car(cdr(C));
   Value decls_cons = cdr(cdr(C));

   DefineLibrarySetup dl;
   try
      {
      dl.key = library_name_to_key(name_sexpr);
      }
   catch (const std::runtime_error& e)
      {
      throw SchemeSyntaxError(
          std::string("define-library: ") + e.what(), src_of(C));
      }
   dl.lib_env = gc_alloc_environment(nullptr);
   Value d = decls_cons;
   while (is_cons(d))
      {
      _process_one_lib_decl(car(d), dl.lib_env, dl.export_names,
                            dl.eval_forms, dl.import_sets, ctx);
      d = cdr(d);
      }
   return dl;
   }

// Build the exports env from export_pairs + lib_env and register the library.
// Run by FRAME_LIB_FINALIZE after the library's forms have evaluated on the main
// loop (so exported names are defined).  export_pairs[i] = (internal sid,
// external symbol Value); name_src = the define-library form (for error src).
static void finalize_define_library(
    Environment* lib_env,
    const std::vector<std::pair<uint32_t, Value>>& export_pairs,
    const std::string& key, const Value& name_src)
   {
   Environment* exports_env = gc_alloc_environment(nullptr);
   gc_env_root_push(&exports_env);
   for (const auto& ep : export_pairs)
      {
      uint32_t internal_sid = ep.first;
      std::string external_name = as_symbol(ep.second);
      if (lib_env->_bindings.count(internal_sid) == 0)
         {
         gc_env_root_pop(&exports_env);
         throw SchemeSyntaxError(
             "define-library: exported name not defined: " + symbol_name(internal_sid),
             src_of(name_src));
         }
      Value val = lib_env->_bindings.at(internal_sid);
      exports_env->bind(external_name, val);
      // A macro's free-identifier aliases (gensyms bound to the library's
      // def-time values in lib_env, the library's parentless "global") are
      // part of its hygiene closure.  Carry them into the exports env so they
      // travel with the import and the macro's template references resolve at
      // the use site (R7RS 4.3 referential transparency; mirrors pyScheme).
      if (is_syntax_transformer(val))
         {
         SyntaxTransformer* t = std::get<SyntaxTransformer*>(val.repr);
         for (const auto& kv : t->free_id_map)
            {
            uint32_t gs_sid = kv.second;
            if (lib_env->_bindings.count(gs_sid) && exports_env->_bindings.count(gs_sid) == 0)
               exports_env->bind(symbol_name(gs_sid), lib_env->_bindings.at(gs_sid));
            }
         }
      }
   exports_env->freeze();
   library_register(key, exports_env);
   gc_env_root_pop(&exports_env);
   }

// ── The CEK machine ───────────────────────────────────────────────────────────

// Special-form dispatch kinds (optimization #1).  KW::classify() maps a head
// symbol's interned id to one of these in O(1); SF_NONE means "not a syntactic
// keyword" and takes the application fast-path.  The eval loop switches on the
// kind, so an application skips the whole keyword ladder.
enum SpecialForm : uint8_t
   {
   SF_NONE = 0,
   SF_QUOTE, SF_LAMBDA, SF_CASE_LAMBDA, SF_DELAY, SF_DELAY_FORCE,
   SF_IMPORT, SF_DEFINE_LIBRARY, SF_IF, SF_DEFINE, SF_SET,
   SF_BEGIN, SF_WHEN, SF_UNLESS, SF_AND, SF_OR,
   SF_COND, SF_CASE, SF_LET, SF_LET_STAR, SF_LETREC,
   SF_TRACE, SF_UNTRACE
   };

static Value cek_loop(const Value& expr, Environment* env, Context* ctx)
   {
   // Intern all keyword symbol IDs once.
   struct KW
      {
      uint32_t quote, lambda, case_lambda, delay, delay_force;
      uint32_t import_, define_library, if_, define, set_;
      uint32_t begin, when, unless, and_, or_;
      uint32_t cond, case_, let, let_star, letrec, letrec_star;
      uint32_t trace, untrace;
      std::vector<uint8_t> kind_by_sid;   // sid -> SpecialForm (Opt #1)
      SpecialForm classify(uint32_t sid) const
         {
         return sid < kind_by_sid.size()
                   ? (SpecialForm)kind_by_sid[sid] : SF_NONE;
         }
      KW()
         {
         quote = intern_symbol("quote");
         lambda = intern_symbol("lambda");
         case_lambda = intern_symbol("case-lambda");
         delay = intern_symbol("delay");
         delay_force = intern_symbol("delay-force");
         import_ = intern_symbol("import");
         define_library = intern_symbol("define-library");
         if_ = intern_symbol("if");
         define = intern_symbol("define");
         set_ = intern_symbol("set!");
         begin = intern_symbol("begin");
         when = intern_symbol("when");
         unless = intern_symbol("unless");
         and_ = intern_symbol("and");
         or_ = intern_symbol("or");
         cond = intern_symbol("cond");
         case_ = intern_symbol("case");
         let = intern_symbol("let");
         let_star = intern_symbol("let*");
         letrec = intern_symbol("letrec");
         letrec_star = intern_symbol("letrec*");
         trace = intern_symbol("trace");
         untrace = intern_symbol("untrace");
         uint32_t mx = 0;
         for (uint32_t s : { quote, lambda, case_lambda, delay, delay_force,
                             import_, define_library, if_, define, set_,
                             begin, when, unless, and_, or_, cond, case_,
                             let, let_star, letrec, letrec_star, trace, untrace })
            if (s > mx)
               mx = s;
         kind_by_sid.assign(mx + 1, (uint8_t)SF_NONE);
         kind_by_sid[quote] = SF_QUOTE;
         kind_by_sid[lambda] = SF_LAMBDA;
         kind_by_sid[case_lambda] = SF_CASE_LAMBDA;
         kind_by_sid[delay] = SF_DELAY;
         kind_by_sid[delay_force] = SF_DELAY_FORCE;
         kind_by_sid[import_] = SF_IMPORT;
         kind_by_sid[define_library] = SF_DEFINE_LIBRARY;
         kind_by_sid[if_] = SF_IF;
         kind_by_sid[define] = SF_DEFINE;
         kind_by_sid[set_] = SF_SET;
         kind_by_sid[begin] = SF_BEGIN;
         kind_by_sid[when] = SF_WHEN;
         kind_by_sid[unless] = SF_UNLESS;
         kind_by_sid[and_] = SF_AND;
         kind_by_sid[or_] = SF_OR;
         kind_by_sid[cond] = SF_COND;
         kind_by_sid[case_] = SF_CASE;
         kind_by_sid[let] = SF_LET;
         kind_by_sid[let_star] = SF_LET_STAR;
         kind_by_sid[letrec] = SF_LETREC;
         kind_by_sid[letrec_star] = SF_LETREC;
         kind_by_sid[trace] = SF_TRACE;
         kind_by_sid[untrace] = SF_UNTRACE;
         }
      };
   static const KW kw;

   Value C = expr;
   Value V;
   Environment* E = env;
   KStack K;
   bool skip_eval = false;

   // Register live CEK state as GC roots so the collector can trace and
   // forward everything reachable from C, V, E, K, and the Context stacks.
   gc_push_trace_hook([&]()
                      {
        gc_trace_value(C);
        gc_trace_value(V);
        gc_trace_environment(E);
        for (Frame& f : K) {
            gc_trace_value(f.v1);
            gc_trace_value(f.v2);
            if (f.env) gc_trace_environment(f.env);
            for (Value& v : f.list1) gc_trace_value(v);
            for (Value& v : f.list2) gc_trace_value(v);
            for (auto& p : f.pairs) gc_trace_value(p.second);
        }
        for (Value& h : ctx->handler_stack) gc_trace_value(h);
        for (WindFrame& wf : ctx->wind_stack) {
            gc_trace_value(wf.before);
            gc_trace_value(wf.after);
        } });
   gc_push_forward_hook([&]()
                        {
        gc_forward_value(C);
        gc_forward_value(V);
        gc_copy_forward_env(E);
        for (Frame& f : K) {
            gc_forward_value(f.v1);
            gc_forward_value(f.v2);
            gc_copy_forward_env(f.env);
            for (Value& v : f.list1) gc_forward_value(v);
            for (Value& v : f.list2) gc_forward_value(v);
            for (auto& p : f.pairs) gc_forward_value(p.second);
        }
        for (Value& h : ctx->handler_stack) gc_forward_value(h);
        for (WindFrame& wf : ctx->wind_stack) {
            gc_forward_value(wf.before);
            gc_forward_value(wf.after);
        } });
   struct HookGuard
      {
      ~HookGuard()
         {
         gc_pop_forward_hook();
         gc_pop_trace_hook();
         }
      } _hook_guard;

   // Helper: dispatch handler on exception.
   // Returns true if handler found + state updated; false = re-raise.
   auto dispatch_exc = [&](Value raised_value, bool is_scheme_raised, bool continuable) -> bool
   {
      // The condition object is held while the FRAME_ERROR_UNWIND frame / handler
      // dispatch is set up; root it so a minor GC cannot leave it stale before it
      // is stored into a frame / passed to the handler.
      GcRootGuard raised_root(raised_value);
      Value handler_val;
      bool found = false;
      bool is_guard_handler = false;
      // Walk K ONCE to find a handler frame, COLLECTING (not running) the
      // FRAME_DYNAMIC_WIND_AFTER thunks for the extents between the raise and the
      // handler.  A single scan keeps the reinstall accounting correct; the
      // collected afters are then run on the K stack by FRAME_ERROR_UNWIND (so an
      // after-thunk's continuation/HOF no longer re-enters cek_eval), which
      // performs the terminal action -- dispatch handler, or re-raise if none.
      // Propagate semantics: an after that raises becomes the new in-flight
      // condition (matches Chez; R7RS-unspecified).
      std::vector<Value> unwind_afters;
      // A raise-continuable pops its handler off handler_stack but leaves the
      // handler's FRAME_POP_HANDLER / FRAME_GUARD on K, with a
      // FRAME_REINSTALL_HANDLER above it.  When the handler then raises and we
      // unwind, those orphaned frames must be skipped WITHOUT popping
      // handler_stack, or K frames and handler_stack drift out of alignment and
      // we pair a frame with the wrong handler (e.g. treat a guard handler as a
      // plain one and spuriously raise "exception handler returned").  Count
      // the FRAME_REINSTALL_HANDLER frames and skip that many handler frames.
      int pending_reinstalls = 0;
      while (!K.empty())
         {
         Frame f = std::move(K.back());
         K.pop_back();
         if (f.tag == FRAME_REINSTALL_HANDLER)
            {
            ++pending_reinstalls;
            continue;
            }
         if (f.tag == FRAME_POP_HANDLER)
            {
            if (pending_reinstalls > 0)
               {
               --pending_reinstalls;
               continue;
               }
            if (ctx->handler_stack.empty())
               break;
            if (!unwind_afters.empty())
               {
               // Leave the handler installed: the collected afters run within its
               // dynamic extent, so an after that raises must reach it.
               // FRAME_ERROR_UNWIND re-raises once the afters are done, and this
               // frame -- now atop K -- is dispatched with no afters.
               K.push_back(std::move(f));
               break;
               }
            handler_val = std::move(ctx->handler_stack.back());
            ctx->handler_stack.pop_back();
            found = true;
            break;
            }
         if (f.tag == FRAME_GUARD)
            {
            if (pending_reinstalls > 0)
               {
               --pending_reinstalls;
               continue;
               }
            if (ctx->handler_stack.empty())
               break;
            if (!unwind_afters.empty())
               {
               K.push_back(std::move(f));
               break;
               }
            handler_val = std::move(ctx->handler_stack.back());
            ctx->handler_stack.pop_back();
            found = true;
            is_guard_handler = true;
            break;
            }
         if (f.tag == FRAME_DYNAMIC_WIND_AFTER)
            {
            if (!ctx->wind_stack.empty())
               ctx->wind_stack.pop_back();
            unwind_afters.push_back(f.v1);
            }
         }
      if (!unwind_afters.empty())
         {
         // Run the collected afters on the K stack, then re-raise (the handler
         // frame, if any, was left installed above and is dispatched then).
         // skip_eval: resume in the APPLY phase to process FRAME_ERROR_UNWIND;
         // WITHOUT this the loop would re-EVAL the unchanged C -- an infinite loop.
         Frame eu;
         eu.tag = FRAME_ERROR_UNWIND;
         eu.list1 = std::move(unwind_afters);
         eu.depth = 0;
         eu.v1 = raised_root.val;          // GC-root the condition across the afters
         eu.exc = std::current_exception(); // faithfully re-raised when afters done
         K.push_back(std::move(eu));
         skip_eval = true;
         return true;
         }
      if (!found)
         return false;
      // No winds to run: dispatch the handler inline.
      if (is_scheme_raised && !continuable && !is_guard_handler)
         {
         Frame nf;
         nf.tag = FRAME_NONCONTIN_RETURN;
         nf.v1 = raised_root.val;
         K.push_back(std::move(nf));
         }
      std::vector<Value> hargs = {raised_root.val};
      EnterResult result = enter_proc(handler_val, hargs, ctx, E, nullptr);
      if (apply_enter_result(result, K, V, C, E) == EnterAction::DoApply)
         {
         // Handler produced a value, or is itself a frame-driven HOF primitive
         // whose driver was just pushed; resume the APPLY loop (the start-frame
         // ignores the current V).
         skip_eval = true;
         }
      // else DoEval: handler is a closure body; C and E are set and skip_eval
      // stays false so the loop evaluates it.
      return true;
   };

   for (;;)
      { // A: exception restart loop
      try
         {
         for (;;)
            { // B: main state machine
            ctx->_timeout_step = (ctx->_timeout_step + 1) & 0xFFFFu;
            if (ctx->_timeout_step == 0 && ctx->timeout_active &&
                SteadyClock::now() > ctx->timeout_at)
               throw std::runtime_error("Evaluation timed out.");
            if ((ctx->_timeout_step & 0x3FFu) == 0 && gc_needs_collection())
               gc_collect();

            // ── EVAL phase (loop C) ────────────────────────────────────────
            for (;;)
               {
               if (skip_eval)
                  {
                  skip_eval = false;
                  break;
                  }

               if (is_cons(C))
                  {
                  if (ctx->_instrumented && ctx->debugger)
                     ctx->debugger->on_expr(C, E, K, ctx);

                  Value head = car(C);

                  if (is_symbol(head))
                     {
                     // Opt #1: classify the head's interned id once.  SF_NONE
                     // (an application - the hot case) skips the switch and
                     // falls through to the application path below; a keyword
                     // dispatches through the jump table.
                     SpecialForm sf = kw.classify(as_symbol_id(head));
                     if (sf != SF_NONE)
                        {
                        switch (sf)
                           {
                           case SF_QUOTE:
                              {
                              V = car(cdr(C));
                              mark_literal_immutable(V);
                              goto eval_break;
                              }
                           case SF_LAMBDA:
                              {
                              V = make_closure_from_lambda(C, E);
                              goto eval_break;
                              }
                           case SF_CASE_LAMBDA:
                              {
                              V = make_case_closure_from_form(C, E);
                              goto eval_break;
                              }
                           case SF_DELAY:
                           case SF_DELAY_FORCE:
                              {
                              Value ex = car(cdr(C));
                              Value body = alloc_cons(ex, NIL_VALUE);
                              Value thunk = make_closure({}, body, E, UINT32_MAX, "");
                              // delay-force tail-chases into a promise result;
                              // plain delay returns its value as-is (R7RS 4.2.5).
                              bool iterative = (sf == SF_DELAY_FORCE);
                              V = make_promise_lazy(thunk, iterative);
                              goto eval_break;
                              }
                           case SF_IMPORT:
                              {
                              // (import <import-set>...) -- resolve each set and
                              // bind its exported names into the current env,
                              // loading library files on the K stack if needed.
                              // Driven by FRAME_IMPORT_STEP (no re-entrant cek_eval).
                              Frame im;
                              im.tag = FRAME_IMPORT_STEP;
                              for (Value s = cdr(C); is_cons(s); s = cdr(s))
                                 im.list1.push_back(car(s));
                              im.depth = 0;
                              im.env = E;
                              im.uid = 0; // post_load = false
                              im.str1 = "import: ";
                              K.push_back(std::move(im));
                              V = VOID_VALUE;
                              goto eval_break;
                              }
                           case SF_DEFINE_LIBRARY:
                              {
                              // (define-library <name> <decl>...): the pre-pass
                              // resolves the pure decls and collects the import
                              // sets + begin/unknown forms; those run on the main
                              // loop (FRAME_IMPORT_STEP then FRAME_EVAL_FORMS) with
                              // the runtime env swapped to lib_env -- restored via a
                              // wind so it resets on normal return, error, and
                              // continuation escape -- then FRAME_LIB_FINALIZE
                              // builds + registers exports.  No re-entrant cek_eval.
                              DefineLibrarySetup dl = define_library_setup(C, ctx);
                              Environment* outer_env = get_runtime_env();
                              Value install = make_native_closure(
                                  "%define-library-install-env", nc_set_runtime_env,
                                  {make_environment(dl.lib_env)});
                              Value restore = make_native_closure(
                                  "%define-library-restore-env", nc_set_runtime_env,
                                  {make_environment(outer_env)});
                              set_runtime_env(dl.lib_env);
                              WindFrame wf;
                              wf.before = install;
                              wf.after = restore;
                              ctx->wind_stack.push_back(wf);
                              Frame da;
                              da.tag = FRAME_DYNAMIC_WIND_AFTER;
                              da.v1 = restore;
                              K.push_back(std::move(da));
                              Frame lf;
                              lf.tag = FRAME_LIB_FINALIZE;
                              lf.env = dl.lib_env;
                              lf.str1 = dl.key;
                              lf.v1 = C;
                              for (const auto& en : dl.export_names)
                                 lf.pairs.push_back(
                                     {intern_symbol(en.first), make_symbol(en.second)});
                              K.push_back(std::move(lf));
                              Frame ef;
                              ef.tag = FRAME_EVAL_FORMS;
                              ef.list1 = std::move(dl.eval_forms);
                              ef.env = dl.lib_env;
                              ef.depth = 0;
                              K.push_back(std::move(ef));
                              // Imports run first (frame-driven, loading library
                              // files on the K stack if needed), before the begin
                              // forms; pushed last so APPLY pops it first.
                              Frame im;
                              im.tag = FRAME_IMPORT_STEP;
                              im.list1 = std::move(dl.import_sets);
                              im.depth = 0;
                              im.env = dl.lib_env;
                              im.uid = 0; // post_load = false
                              im.str1 = "define-library: import: ";
                              K.push_back(std::move(im));
                              V = VOID_VALUE;
                              goto eval_break;
                              }
                           case SF_IF:
                              {
                              Frame f;
                              f.tag = FRAME_IF;
                              f.v1 = car(cdr(cdr(C)));      // then
                              f.v2 = car(cdr(cdr(cdr(C)))); // else
                              f.env = E;
                              K.push_back(std::move(f));
                              C = car(cdr(C));
                              continue;
                              }
                           case SF_DEFINE:
                              {
                              Frame f;
                              f.tag = FRAME_DEFINE;
                              f.v1 = car(cdr(C));
                              f.env = E;
                              K.push_back(std::move(f));
                              C = car(cdr(cdr(C)));
                              continue;
                              }
                           case SF_SET:
                              {
                              Value name_sexpr = car(cdr(C));
                              Frame f;
                              f.tag = FRAME_SET;
                              f.v1 = name_sexpr;
                              f.env = E;
                              f.src_ptr = src_of(name_sexpr);
                              K.push_back(std::move(f));
                              C = car(cdr(cdr(C)));
                              continue;
                              }
                           case SF_BEGIN:
                              {
                              Value body = cdr(C);
                              if (is_nil(body))
                                 {
                                 V = VOID_VALUE;
                                 goto eval_break;
                                 }
                              C = car(body);
                              if (is_cons(cdr(body)))
                                 {
                                 Frame f;
                                 f.tag = FRAME_SEQ;
                                 f.v1 = cdr(body);
                                 f.env = E;
                                 K.push_back(std::move(f));
                                 }
                              continue;
                              }
                           case SF_WHEN:
                              {
                              Frame f;
                              f.tag = FRAME_WHEN;
                              f.v1 = cdr(cdr(C));
                              f.env = E;
                              K.push_back(std::move(f));
                              C = car(cdr(C));
                              continue;
                              }
                           case SF_UNLESS:
                              {
                              Frame f;
                              f.tag = FRAME_UNLESS;
                              f.v1 = cdr(cdr(C));
                              f.env = E;
                              K.push_back(std::move(f));
                              C = car(cdr(C));
                              continue;
                              }
                           case SF_AND:
                              {
                              Value body = cdr(C);
                              if (is_nil(body))
                                 {
                                 V = make_boolean(true);
                                 goto eval_break;
                                 }
                              if (is_cons(cdr(body)))
                                 {
                                 Frame f;
                                 f.tag = FRAME_AND;
                                 f.v1 = cdr(body);
                                 f.env = E;
                                 K.push_back(std::move(f));
                                 }
                              C = car(body);
                              continue;
                              }
                           case SF_OR:
                              {
                              Value body = cdr(C);
                              if (is_nil(body))
                                 {
                                 V = make_boolean(false);
                                 goto eval_break;
                                 }
                              if (is_cons(cdr(body)))
                                 {
                                 Frame f;
                                 f.tag = FRAME_OR;
                                 f.v1 = cdr(body);
                                 f.env = E;
                                 K.push_back(std::move(f));
                                 }
                              C = car(body);
                              continue;
                              }
                           case SF_COND:
                              {
                              Value clauses = cdr(C);
                              Value first = car(clauses);
                              Value rest = cdr(clauses);
                              CondClause kind = classify_cond_clause(first, E);
                              if (kind.kind == CondClause::Else)
                                 {
                                 Value body = kind.body_cons;
                                 C = car(body);
                                 if (is_cons(cdr(body)))
                                    {
                                    Frame f;
                                    f.tag = FRAME_SEQ;
                                    f.v1 = cdr(body);
                                    f.env = E;
                                    K.push_back(std::move(f));
                                    }
                                 continue;
                                 }
                              Frame f;
                              f.tag = FRAME_COND;
                              f.v1 = first;
                              f.v2 = rest;
                              f.env = E;
                              K.push_back(std::move(f));
                              C = kind.test;
                              continue;
                              }
                           case SF_CASE:
                              {
                              Value clauses = cdr(cdr(C));
                              Frame f;
                              f.tag = FRAME_CASE;
                              f.v1 = car(clauses);
                              f.v2 = cdr(clauses);
                              f.env = E;
                              K.push_back(std::move(f));
                              C = car(cdr(C));
                              continue;
                              }
                           case SF_LET:
                              {
                              if (is_symbol(car(cdr(C))))
                                 {
                                 // named let
                                 Value loop_name_sym = car(cdr(C));
                                 uint32_t loop_nm_id = as_symbol_id(loop_name_sym);
                                 Value bindings_cons = car(cdr(cdr(C)));
                                 Value body_cons = cdr(cdr(cdr(C)));
                                 auto pairs = collect_let_bindings(bindings_cons);
                                 std::vector<uint32_t> param_ids;
                                 std::vector<Value> init_exprs;
                                 for (auto& p : pairs)
                                    {
                                    param_ids.push_back(p.first);
                                    init_exprs.push_back(p.second);
                                    }
                                 Environment* loop_env = gc_alloc_environment(E);
                                 loop_env->bind_id(loop_nm_id, VOID_VALUE);
                                 Value closure = make_closure(param_ids, body_cons, loop_env, UINT32_MAX, "");
                                 loop_env->bind_id(loop_nm_id, closure);
                                 V = closure;
                                 Frame f;
                                 f.tag = FRAME_ARG;
                                 f.list1 = std::move(init_exprs);
                                 f.env = loop_env;
                                 f.v1 = C;
                                 K.push_back(std::move(f));
                                 goto eval_break;
                                 }
                              Value bindings_cons = car(cdr(C));
                              Value body_cons = cdr(cdr(C));
                              auto pairs = collect_let_bindings(bindings_cons);
                              if (pairs.empty())
                                 {
                                 C = car(body_cons);
                                 if (is_cons(cdr(body_cons)))
                                    {
                                    Frame f;
                                    f.tag = FRAME_SEQ;
                                    f.v1 = cdr(body_cons);
                                    f.env = E;
                                    K.push_back(std::move(f));
                                    }
                                 continue;
                                 }
                              std::vector<uint32_t> names;
                              std::vector<Value> val_exprs;
                              for (auto& p : pairs)
                                 {
                                 names.push_back(p.first);
                                 val_exprs.push_back(p.second);
                                 }
                              std::vector<Value> remaining(val_exprs.begin() + 1, val_exprs.end());
                              Frame f;
                              f.tag = FRAME_LET;
                              f.ids = std::move(names);
                              f.list1 = {};
                              f.list2 = std::move(remaining);
                              f.v1 = body_cons;
                              f.env = E;
                              K.push_back(std::move(f));
                              C = val_exprs[0];
                              continue;
                              }
                           case SF_LET_STAR:
                              {
                              Value bindings_cons = car(cdr(C));
                              Value body_cons = cdr(cdr(C));
                              auto pairs = collect_let_bindings(bindings_cons);
                              if (pairs.empty())
                                 {
                                 C = car(body_cons);
                                 if (is_cons(cdr(body_cons)))
                                    {
                                    Frame f;
                                    f.tag = FRAME_SEQ;
                                    f.v1 = cdr(body_cons);
                                    f.env = E;
                                    K.push_back(std::move(f));
                                    }
                                 continue;
                                 }
                              std::vector<std::pair<uint32_t, Value>> remaining(pairs.begin() + 1, pairs.end());
                              Frame f;
                              f.tag = FRAME_LET_STAR;
                              f.uid = pairs[0].first;
                              f.pairs = std::move(remaining);
                              f.v1 = body_cons;
                              f.env = E;
                              K.push_back(std::move(f));
                              C = pairs[0].second;
                              continue;
                              }
                           case SF_LETREC:
                              {
                              Value bindings_cons = car(cdr(C));
                              Value body_cons = cdr(cdr(C));
                              auto pairs = collect_let_bindings(bindings_cons);
                              if (pairs.empty())
                                 {
                                 C = car(body_cons);
                                 if (is_cons(cdr(body_cons)))
                                    {
                                    Frame f;
                                    f.tag = FRAME_SEQ;
                                    f.v1 = cdr(body_cons);
                                    f.env = E;
                                    K.push_back(std::move(f));
                                    }
                                 continue;
                                 }
                              Environment* new_env = gc_alloc_environment(E);
                              for (auto& p : pairs)
                                 new_env->bind_id(p.first, VOID_VALUE);
                              std::vector<std::pair<uint32_t, Value>> remaining(pairs.begin() + 1, pairs.end());
                              Frame f;
                              f.tag = FRAME_LETREC;
                              f.uid = pairs[0].first;
                              f.pairs = std::move(remaining);
                              f.v1 = body_cons;
                              f.env = new_env;
                              K.push_back(std::move(f));
                              C = pairs[0].second;
                              E = new_env;
                              continue;
                              }
                           case SF_TRACE:
                              {
                              Tracer* trc = ctx->tracer;
                              Value args_cons = cdr(C);
                              if (is_nil(args_cons))
                                 {
                                 V = sorted_sym_list(trc->get_fns());
                                 goto eval_break;
                                 }
                              Value cur2 = args_cons;
                              while (is_cons(cur2))
                                 {
                                 Value sym = car(cur2);
                                 if (!is_symbol(sym))
                                    throw SchemeTypeError("trace: arguments must be symbols", src_of(C));
                                 trc->add_fn(as_symbol(sym));
                                 cur2 = cdr(cur2);
                                 }
                              V = sorted_sym_list(trc->get_fns());
                              goto eval_break;
                              }
                           case SF_UNTRACE:
                              {
                              Tracer* trc = ctx->tracer;
                              Value args_cons = cdr(C);
                              if (is_nil(args_cons))
                                 {
                                 trc->remove_all();
                                 }
                              else
                                 {
                                 Value cur2 = args_cons;
                                 while (is_cons(cur2))
                                    {
                                    Value sym = car(cur2);
                                    if (!is_symbol(sym))
                                       throw SchemeTypeError("untrace: arguments must be symbols", src_of(C));
                                    trc->remove_fn(as_symbol(sym));
                                    cur2 = cdr(cur2);
                                    }
                                 }
                              V = sorted_sym_list(trc->get_fns());
                              goto eval_break;
                              }
                           default:
                              break;
                           }
                        }
                     }
                     // Application (non-keyword symbol or non-symbol head)
                     {
                     std::vector<Value> args = collect_cons_to_list(cdr(C));
                     Frame f;
                     f.tag = FRAME_ARG;
                     f.list1 = std::move(args);
                     f.env = E;
                     f.v1 = C;
                     K.push_back(std::move(f));
                     C = head;
                     continue;
                     }
                  }

               if (is_symbol(C))
                  {
                  uint32_t sym_id = as_symbol_id(C);
                  try
                     {
                     V = E->lookup_id(sym_id);
                     }
                  catch (SchemeUnboundError& e)
                     {
                     if (!e.src)
                        {
                        SourceInfo* s = src_of(C);
                        if (s)
                           e.src = new SourceInfo(*s);
                        }
                     throw;
                     }
                  break;
                  }

               // Self-evaluating
               V = C;
               break;

               // Opt #1 value-producing special forms jump here to leave the
               // EVAL phase (a plain `break` inside the switch would only break
               // the switch).  V already holds the produced value.
               eval_break:
               break;
               } // end EVAL loop C

            // ── APPLY phase (loop D) ───────────────────────────────────────
            for (;;)
               {
               if (K.empty())
                  return V;

               Frame frame = std::move(K.back());
               K.pop_back();
               int ftag = frame.tag;

               if (is_multi_values(V) && !is_mv_ok(ftag))
                  throw SchemeTypeError("multiple values delivered to a single-value context",
                                        src_of(V));

               // ── FRAME_DEFINE ──────────────────────────────────────────
               if (ftag == FRAME_DEFINE)
                  {
                  E = frame.env;
                  try
                     {
                     E->bind_id(as_symbol_id(frame.v1), V);
                     }
                  catch (SchemeTypeError& e) // define into a frozen environment
                     {
                     if (!e.src)
                        {
                        SourceInfo* s = src_of(frame.v1);
                        if (s)
                           e.src = new SourceInfo(*s);
                        }
                     throw;
                     }
                  V = VOID_VALUE;
                  continue;
                  }

               // ── FRAME_SET ─────────────────────────────────────────────
               if (ftag == FRAME_SET)
                  {
                  E = frame.env;
                  try
                     {
                     E->set_id(as_symbol_id(frame.v1), V);
                     }
                  // set_id throws SchemeUnboundError (no binding) or
                  // SchemeTypeError (frozen environment); both are positioned.
                  catch (PositionedSchemeError& e)
                     {
                     if (!e.src)
                        {
                        SourceInfo* s = frame.src_ptr;
                        if (s)
                           e.src = new SourceInfo(*s);
                        }
                     throw;
                     }
                  V = VOID_VALUE;
                  continue;
                  }

               // ── FRAME_IF ──────────────────────────────────────────────
               if (ftag == FRAME_IF)
                  {
                  C = is_false_val(V) ? frame.v2 : frame.v1;
                  E = frame.env;
                  break;
                  }

               // ── FRAME_DYNAMIC_WIND_AFTER ──────────────────────────────
               if (ftag == FRAME_DYNAMIC_WIND_AFTER)
                  {
                  // The body has produced its value (now in V).  Pop the wind
                  // entry, then run after_thunk ON THE K STACK (via enter_proc,
                  // not a re-entrant apply_scheme_proc) for its effect,
                  // preserving the body result across it with FRAME_RESTORE_VALUE.
                  Value after = frame.v1;
                  if (!ctx->wind_stack.empty())
                     ctx->wind_stack.pop_back();
                  Frame rv;
                  rv.tag = FRAME_RESTORE_VALUE;
                  rv.v1 = V;
                  K.push_back(std::move(rv));
                  std::vector<Value> ea;
                  EnterResult result = enter_proc(after, ea, ctx, nullptr, nullptr);
                  if (apply_enter_result(result, K, V, C, E) == EnterAction::DoApply)
                     continue;
                  break;
                  }

               // ── FRAME_RESTORE_VALUE ───────────────────────────────────
               if (ftag == FRAME_RESTORE_VALUE)
                  {
                  // Discard the incoming V (a wind after-thunk's result) and
                  // reinstate the value saved when the frame was pushed.
                  V = frame.v1;
                  continue;
                  }

               // ── FRAME_DYNAMIC_WIND_BEFORE_DONE ────────────────────────
               if (ftag == FRAME_DYNAMIC_WIND_BEFORE_DONE)
                  {
                  // The before-thunk has completed (V is its result, discarded).
                  // Now the dynamic extent is active: install the wind entry and
                  // after-frame, then tail-call the body thunk on the K stack.
                  Value before = frame.v1;
                  Value thunk = frame.v2;
                  Value after = frame.list1[0];
                  WindFrame wf;
                  wf.before = before;
                  wf.after = after;
                  ctx->wind_stack.push_back(wf);
                  Frame df;
                  df.tag = FRAME_DYNAMIC_WIND_AFTER;
                  df.v1 = after;
                  K.push_back(std::move(df));
                  std::vector<Value> ea;
                  EnterResult result = enter_proc(thunk, ea, ctx, nullptr, nullptr);
                  if (apply_enter_result(result, K, V, C, E) == EnterAction::DoApply)
                     continue;
                  break;
                  }

               // ── FRAME_WIND_STEP ───────────────────────────────────────
               if (ftag == FRAME_WIND_STEP)
                  {
                  // Drives a continuation jump's wind walk on the K stack: each op
                  // exits an extent (pop wind_stack, run its after) or enters one
                  // (push wind_stack, run its before), with the thunk run via
                  // enter_proc rather than a re-entrant apply_scheme_proc.  The
                  // incoming V (a wind thunk's result) is discarded.  When the ops
                  // are exhausted, install the continuation: restore the handler /
                  // shadow stacks, swap in its K, and deliver its value.
                  size_t w_i = (size_t)frame.depth;
                  if (w_i < frame.ids.size())
                     {
                     Value w_thunk;
                     if (frame.ids[w_i] == 0) // exit
                        {
                        if (!ctx->wind_stack.empty())
                           ctx->wind_stack.pop_back();
                        w_thunk = frame.list1[w_i];
                        }
                     else // enter
                        {
                        WindFrame wf;
                        wf.before = frame.list1[w_i];
                        wf.after = frame.list2[w_i];
                        ctx->wind_stack.push_back(wf);
                        w_thunk = frame.list1[w_i];
                        }
                     Frame nf = frame;          // re-push self at index+1
                     nf.depth = (int)w_i + 1;
                     K.push_back(std::move(nf));
                     std::vector<Value> ea;
                     EnterResult result = enter_proc(w_thunk, ea, ctx, nullptr, nullptr);
                     if (apply_enter_result(result, K, V, C, E) == EnterAction::DoApply)
                        continue;
                     break;
                     }
                  // All wind thunks have run: install the continuation.
                  restore_handler_stack(ctx, as_continuation_handlers(frame.v1));
                  K = *static_cast<const KStack*>(as_continuation_frames(frame.v1));
                  restore_shadow_stack(ctx, as_continuation_shadow(frame.v1));
                  V = frame.v2;
                  continue;
                  }

               // ── FRAME_PARAMETERIZE_STEP ───────────────────────────────
               if (ftag == FRAME_PARAMETERIZE_STEP)
                  {
                  // Drives parameterize's value converters on the K stack: for each
                  // parameter with a converter, tail-call it with the raw value and
                  // collect the result; parameters without a converter take the raw
                  // value directly.  When every value is converted, install the winds
                  // (FRAME_DYNAMIC_WIND_AFTER + wind_stack) and tail-call the body
                  // thunk.  uid != 0 (awaiting) means V holds the converter result
                  // for params[index] and must be collected.
                  std::vector<Value>& p_params = frame.list1;
                  std::vector<Value>& p_raw = frame.list2;
                  std::vector<Value> p_acc;
                  for (auto& pr : frame.pairs)
                     p_acc.push_back(pr.second);
                  size_t p_i = (size_t)frame.depth;
                  Value p_thunk = frame.v1;
                  Value p_app = frame.v2;
                  if (frame.uid != 0) // awaiting
                     {
                     p_acc.push_back(V);
                     ++p_i;
                     }
                  // Parameters needing no converter take the raw value directly.
                  while (p_i < p_params.size() &&
                         is_nil(as_parameter_converter(p_params[p_i])))
                     {
                     p_acc.push_back(p_raw[p_i]);
                     ++p_i;
                     }
                  if (p_i < p_params.size())
                     {
                     // params[p_i] has a converter: run it on the K stack.
                     Value conv = as_parameter_converter(p_params[p_i]);
                     Frame nf;
                     nf.tag = FRAME_PARAMETERIZE_STEP;
                     nf.list1 = p_params;
                     nf.list2 = p_raw;
                     for (Value& a : p_acc)
                        nf.pairs.push_back({0u, a});
                     nf.depth = (int)p_i;
                     nf.uid = 1; // awaiting
                     nf.v1 = p_thunk;
                     nf.v2 = p_app;
                     K.push_back(std::move(nf));
                     std::vector<Value> ca = {p_raw[p_i]};
                     EnterResult result = enter_proc(conv, ca, ctx, nullptr, &p_app);
                     if (apply_enter_result(result, K, V, C, E) == EnterAction::DoApply)
                        continue;
                     break;
                     }
                  // Every value converted: install winds, tail-call the body.
                  auto pw = finalize_parameterize_winds(p_params, p_acc, ctx);
                  WindFrame wf;
                  wf.before = pw.first;
                  wf.after = pw.second;
                  ctx->wind_stack.push_back(wf);
                  Frame df;
                  df.tag = FRAME_DYNAMIC_WIND_AFTER;
                  df.v1 = pw.second;
                  K.push_back(std::move(df));
                  std::vector<Value> ea;
                  EnterResult result = enter_proc(p_thunk, ea, ctx, nullptr, &p_app);
                  if (apply_enter_result(result, K, V, C, E) == EnterAction::DoApply)
                     continue;
                  break;
                  }

               // ── FRAME_ERROR_UNWIND ────────────────────────────────────
               if (ftag == FRAME_ERROR_UNWIND)
                  {
                  // Runs the dynamic-wind after-thunks for the extents between a
                  // raise and its handler ON THE K STACK (the dispatch_exc scan
                  // collected them in one pass, preserving its reinstall
                  // accounting, and left the handler frame installed below so it
                  // still protects these afters).  Each after's result is
                  // discarded.  When the afters are exhausted, re-raise the
                  // original condition: the still-installed handler frame is now
                  // atop K, so dispatch_exc handles it via its no-afters path.
                  // Propagate semantics (matches Chez): an after that raises
                  // becomes the new in-flight condition, caught by that handler.
                  size_t eu_i = (size_t)frame.depth;
                  if (eu_i < frame.list1.size())
                     {
                     Value after = frame.list1[eu_i];
                     Frame nf = frame;          // re-push self at index+1
                     nf.depth = (int)eu_i + 1;
                     K.push_back(std::move(nf));
                     std::vector<Value> ea;
                     EnterResult result = enter_proc(after, ea, ctx, nullptr, nullptr);
                     if (apply_enter_result(result, K, V, C, E) == EnterAction::DoApply)
                        continue;
                     break;
                     }
                  // Afters exhausted: re-raise the original condition.  For a
                  // SchemeRaised, reinstate the (GC-forwarded) condition Value
                  // from frame.v1 -- a minor GC during the afters may have moved
                  // it, leaving the value stored inside the exception stale.
                  try
                     {
                     std::rethrow_exception(frame.exc);
                     }
                  catch (SchemeRaised& e)
                     {
                     e.value = frame.v1;
                     throw;
                     }
                  }

               // ── FRAME_EVAL_FORMS ──────────────────────────────────────
               if (ftag == FRAME_EVAL_FORMS)
                  {
                  // Evaluates a list of top-level forms in sequence ON THE MAIN K
                  // STACK (load / library loading), instead of a re-entrant
                  // cek_eval per form.  Each form is expanded then evaluated; its
                  // result is discarded.  (C++ does not run the Analyzer pass on
                  // these paths -- matching the old _prim_load / _try_load /
                  // define-library cek_eval(expand(form)) calls.)  Yields VOID when
                  // the forms are exhausted.
                  size_t ef_i = (size_t)frame.depth;
                  if (ef_i >= frame.list1.size())
                     {
                     V = VOID_VALUE;
                     continue;
                     }
                  GcRootGuard raw(frame.list1[ef_i]); // rooted across expand
                  Environment* ef_env = frame.env;
                  Frame nf = std::move(frame);        // preserves list1/env
                  nf.depth = (int)ef_i + 1;
                  K.push_back(std::move(nf));
                  C = expand(raw.val);
                  E = ef_env;
                  break;
                  }

               // ── FRAME_LIB_FINALIZE ────────────────────────────────────
               if (ftag == FRAME_LIB_FINALIZE)
                  {
                  // The library's forms have evaluated on the main loop; build +
                  // register its exports.  Reached only on normal completion (a
                  // library form that raised unwinds past this frame), so a failed
                  // library is not registered.  The runtime-env restore rides a
                  // wind beneath this frame.
                  finalize_define_library(frame.env, frame.pairs, frame.str1, frame.v1);
                  V = VOID_VALUE;
                  continue;
                  }

               // ── FRAME_IMPORT_STEP ─────────────────────────────────────
               if (ftag == FRAME_IMPORT_STEP)
                  {
                  // Resolves each import-set and binds its exports into env.  When
                  // a set names an unregistered library, frame-drives a load
                  // (FRAME_ENSURE_LOADED) then retries (post_load=true), so library
                  // files evaluate on the main K stack instead of a re-entrant
                  // cek_eval.  Used for top-level import and library imports.
                  size_t im_i = (size_t)frame.depth;
                  if (im_i >= frame.list1.size())
                     {
                     V = VOID_VALUE;
                     continue;
                     }
                  Value import_set = frame.list1[im_i];
                  Environment* im_env = frame.env;
                  bool im_post = (frame.uid != 0);
                  std::unordered_map<std::string, Value> bindings;
                  try
                     {
                     bindings = resolve_import_set(import_set);
                     }
                  catch (const std::runtime_error& ie)
                     {
                     if (!im_post && is_cons(import_set))
                        {
                        std::string ikey;
                        bool have_key = true;
                        try
                           {
                           ikey = library_name_to_key(import_set);
                           }
                        catch (const std::exception&)
                           {
                           have_key = false;
                           }
                        if (have_key && !library_registered_p(ikey))
                           {
                           frame.depth = (int)im_i; // retry this set after load
                           frame.uid = 1;           // post_load = true
                           K.push_back(std::move(frame));
                           Frame el;
                           el.tag = FRAME_ENSURE_LOADED;
                           el.str1 = ikey;
                           el.v1 = import_set;
                           el.depth = 0;
                           K.push_back(std::move(el));
                           continue;
                           }
                        }
                     throw SchemeSyntaxError(frame.str1 + ie.what(), src_of(import_set));
                     }
                  for (const auto& [n, val] : bindings)
                     im_env->bind(n, val);
                  frame.depth = (int)im_i + 1;
                  frame.uid = 0;
                  K.push_back(std::move(frame));
                  continue;
                  }

               // ── FRAME_ENSURE_LOADED ───────────────────────────────────
               if (ftag == FRAME_ENSURE_LOADED)
                  {
                  // Walks the load path for a library: per dir drives <key>.sld's
                  // forms on the K stack via FRAME_EVAL_FORMS (a fresh parentless
                  // env), re-checking registration after each dir.  Yields when the
                  // library is registered or the dirs run out.  (C++ omits the
                  // Python .py-extension branch.)
                  const std::string el_key = frame.str1;
                  size_t el_di = (size_t)frame.depth;
                  if (library_registered_p(el_key))
                     {
                     V = VOID_VALUE;
                     continue;
                     }
                  std::vector<std::string> dirs = _library_load_path();
                  if (el_di >= dirs.size())
                     {
                     V = VOID_VALUE;
                     continue;
                     }
                  // key "scheme.base" -> base path "scheme/base"
                  std::string bpath;
                  size_t start = 0;
                  while (true)
                     {
                     size_t dot = el_key.find('.', start);
                     std::string part = el_key.substr(
                         start, dot == std::string::npos ? dot : dot - start);
                     if (!bpath.empty())
                        bpath += '/';
                     bpath += part;
                     if (dot == std::string::npos)
                        break;
                     start = dot + 1;
                     }
                  const std::string& base = dirs[el_di];
                  std::string prefix = base.empty() ? bpath : (base + "/" + bpath);
                  std::string sld = prefix + ".sld";
                  frame.depth = (int)el_di + 1; // advance; re-check after this dir
                  K.push_back(std::move(frame));
                  std::ifstream sld_file(sld);
                  if (sld_file.is_open())
                     {
                     std::string source((std::istreambuf_iterator<char>(sld_file)),
                                        std::istreambuf_iterator<char>());
                     sld_file.close();
                     std::vector<Value> forms = scheme_parse(source, sld);
                     Environment* fresh_env = gc_alloc_environment(nullptr);
                     Frame ef;
                     ef.tag = FRAME_EVAL_FORMS;
                     ef.list1 = std::move(forms);
                     ef.env = fresh_env;
                     ef.depth = 0;
                     K.push_back(std::move(ef));
                     }
                  continue;
                  }

               // ── FRAME_CWV_CONSUMER ────────────────────────────────────
               if (ftag == FRAME_CWV_CONSUMER)
                  {
                  Value consumer = frame.v1;
                  Value app_node = frame.v2;
                  std::vector<Value> consumer_args;
                  if (is_multi_values(V))
                     consumer_args = as_multi_values_list(V);
                  else
                     consumer_args = {V};
                  EnterResult result = enter_proc(consumer, consumer_args, ctx, E, &app_node);
                  if (apply_enter_result(result, K, V, C, E) == EnterAction::DoApply)
                     continue;
                  break;
                  }

               // ── FRAME_HOF_STEP (map / for-each / filter) ──────────────
               if (ftag == FRAME_HOF_STEP)
                  {
                  int mode = frame.depth;
                  bool started = (frame.uid != 0);
                  // The frame was popped off K, so these Values are no longer
                  // rooted by the K trace; pin them across alloc_cons / enter_proc,
                  // which can move objects.
                  GcRootGuard proc(frame.v1);
                  GcRootGuard acc(frame.v2);
                  GcRootGuard app_node(frame.list2[0]);
                  GcRootGuard pending(frame.list2[1]);
                  GcRootGuard v(V);
                  std::vector<Value> cursors = std::move(frame.list1);
                  GcRootVec cursors_root(cursors);
                  if (started)
                     {
                     if (mode == PRIM_MAP)
                        acc.val = alloc_cons(v.val, acc.val);
                     else if (mode == PRIM_FILTER && !is_false_val(v.val))
                        acc.val = alloc_cons(pending.val, acc.val);
                     // PRIM_FOR_EACH discards V.
                     }
                  bool ready = !cursors.empty();
                  for (const Value& c : cursors)
                     if (!is_cons(c)) { ready = false; break; }
                  if (!ready)
                     {
                     for (const Value& c : cursors)
                        if (!is_cons(c) && !is_nil(c))
                           {
                           const char* msg =
                               mode == PRIM_FILTER   ? "filter: list argument must be a proper list"
                             : mode == PRIM_FOR_EACH ? "for-each: list arguments must be proper lists"
                             :                         "map: list arguments must be proper lists";
                           throw SchemeTypeError(msg, src_of(app_node.val));
                           }
                     if (mode == PRIM_FOR_EACH)
                        V = VOID_VALUE;
                     else
                        {
                        // acc is reversed; rebuild in order (no allocation reads).
                        Value res = NIL_VALUE;
                        GcRootGuard res_root(res);
                        for (Value cur = acc.val; is_cons(cur); cur = cdr(cur))
                           res_root.val = alloc_cons(car(cur), res_root.val);
                        V = res_root.val;
                        }
                     continue;
                     }
                  std::vector<Value> row;
                  std::vector<Value> next;
                  for (const Value& c : cursors)
                     {
                     row.push_back(car(c));
                     next.push_back(cdr(c));
                     }
                  Value next_pending = (mode == PRIM_FILTER) ? row[0] : NIL_VALUE;
                  Frame nf;
                  nf.tag = FRAME_HOF_STEP;
                  nf.depth = mode;
                  nf.uid = 1;
                  nf.v1 = proc.val;
                  nf.v2 = acc.val;
                  nf.list1 = std::move(next);
                  nf.list2 = {app_node.val, next_pending};
                  K.push_back(std::move(nf));
                  EnterResult result = enter_proc(proc.val, row, ctx, E, &app_node.val);
                  if (apply_enter_result(result, K, V, C, E) == EnterAction::DoApply)
                     continue;
                  break;
                  }

               // ── FRAME_HOF_STEP_IDX (vector/string -map / -for-each) ────
               if (ftag == FRAME_HOF_STEP_IDX)
                  {
                  int mode = frame.depth;
                  bool started = (frame.uid != 0);
                  bool is_vec = (mode == PRIM_VECTOR_MAP || mode == PRIM_VECTOR_FOR_EACH);
                  bool is_map = (mode == PRIM_VECTOR_MAP || mode == PRIM_STRING_MAP);
                  GcRootGuard proc(frame.v1);
                  GcRootGuard acc(frame.v2);
                  GcRootGuard app_node(frame.list2[0]);
                  GcRootGuard v(V);
                  std::vector<Value> seqs = std::move(frame.list1);
                  GcRootVec seqs_root(seqs);
                  std::vector<uint32_t> positions = std::move(frame.ids);
                  if (started && is_map)
                     {
                     if (mode == PRIM_STRING_MAP && !is_character(v.val))
                        throw SchemeTypeError("string-map: proc must return a character",
                                              src_of(app_node.val));
                     acc.val = alloc_cons(v.val, acc.val);
                     }
                  bool done = false;
                  for (size_t k = 0; k < seqs.size(); ++k)
                     {
                     size_t limit = is_vec ? as_vector_items_const(seqs[k]).size()
                                           : as_string(seqs[k]).size();
                     if (positions[k] >= limit) { done = true; break; }
                     }
                  if (done)
                     {
                     if (mode == PRIM_VECTOR_FOR_EACH || mode == PRIM_STRING_FOR_EACH)
                        V = VOID_VALUE;
                     else if (mode == PRIM_VECTOR_MAP)
                        {
                        std::vector<Value> items;
                        GcRootVec items_root(items);
                        for (Value cur = acc.val; is_cons(cur); cur = cdr(cur))
                           items.push_back(car(cur));
                        std::reverse(items.begin(), items.end());
                        V = make_vector(std::move(items));
                        }
                     else // PRIM_STRING_MAP
                        {
                        std::vector<char32_t> chars;
                        for (Value cur = acc.val; is_cons(cur); cur = cdr(cur))
                           chars.push_back(as_character(car(cur)));
                        std::reverse(chars.begin(), chars.end());
                        std::string s;
                        for (char32_t cp : chars)
                           utf8_encode(s, cp);
                        V = make_string(s);
                        }
                     continue;
                     }
                  std::vector<Value> row;
                  for (size_t k = 0; k < seqs.size(); ++k)
                     {
                     if (is_vec)
                        {
                        row.push_back(as_vector_items_const(seqs[k])[positions[k]]);
                        positions[k] += 1;
                        }
                     else
                        {
                        size_t p = positions[k];
                        char32_t cp = utf8_next(as_string(seqs[k]), p);
                        row.push_back(make_character(cp));
                        positions[k] = (uint32_t)p;
                        }
                     }
                  Frame nf;
                  nf.tag = FRAME_HOF_STEP_IDX;
                  nf.depth = mode;
                  nf.uid = 1;
                  nf.v1 = proc.val;
                  nf.v2 = acc.val;
                  nf.list1 = std::move(seqs);
                  nf.list2 = {app_node.val};
                  nf.ids = std::move(positions);
                  K.push_back(std::move(nf));
                  EnterResult result = enter_proc(proc.val, row, ctx, E, &app_node.val);
                  if (apply_enter_result(result, K, V, C, E) == EnterAction::DoApply)
                     continue;
                  break;
                  }

               // ── FRAME_SEARCH_STEP (member / assoc 3-arg comparator) ────
               if (ftag == FRAME_SEARCH_STEP)
                  {
                  int mode = frame.depth;
                  bool started = (frame.uid != 0);
                  // Frame was popped off K, so pin its Values across enter_proc.
                  GcRootGuard proc(frame.v1);
                  GcRootGuard target(frame.v2);
                  GcRootGuard cursor(frame.list1[0]);
                  GcRootGuard app_node(frame.list2[0]);
                  GcRootGuard v(V);
                  const char* nm = (mode == PRIM_MEMBER) ? "member" : "assoc";
                  if (started)
                     {
                     // v is the comparator's verdict for the entry at cursor.
                     if (!is_false_val(v.val))
                        {
                        // member returns the matching sublist; assoc the pair.
                        V = (mode == PRIM_MEMBER) ? cursor.val : car(cursor.val);
                        continue;
                        }
                     cursor.val = cdr(cursor.val);
                     }
                  if (!is_cons(cursor.val))
                     {
                     if (!is_nil(cursor.val))
                        throw SchemeTypeError(
                            std::string(nm) + ": second argument must be a proper list",
                            src_of(app_node.val));
                     V = make_boolean(false);
                     continue;
                     }
                  Value item;
                  if (mode == PRIM_MEMBER)
                     item = car(cursor.val);
                  else
                     {
                     Value entry = car(cursor.val);
                     if (!is_cons(entry))
                        throw SchemeTypeError(
                            std::string(nm) + ": alist entries must be pairs",
                            src_of(app_node.val));
                     item = car(entry);
                     }
                  Frame nf;
                  nf.tag = FRAME_SEARCH_STEP;
                  nf.depth = mode;
                  nf.uid = 1;
                  nf.v1 = proc.val;
                  nf.v2 = target.val;
                  nf.list1 = {cursor.val};
                  nf.list2 = {app_node.val};
                  K.push_back(std::move(nf));
                  std::vector<Value> row = {target.val, item};
                  EnterResult result = enter_proc(proc.val, row, ctx, E, &app_node.val);
                  if (apply_enter_result(result, K, V, C, E) == EnterAction::DoApply)
                     continue;
                  break;
                  }

               // ── FRAME_POP_HANDLER ─────────────────────────────────────
               if (ftag == FRAME_POP_HANDLER)
                  {
                  if (!ctx->handler_stack.empty())
                     ctx->handler_stack.pop_back();
                  continue;
                  }

               // ── FRAME_GUARD ───────────────────────────────────────────
               if (ftag == FRAME_GUARD)
                  {
                  if (!ctx->handler_stack.empty())
                     ctx->handler_stack.pop_back();
                  continue;
                  }

               // ── FRAME_REINSTALL_HANDLER ───────────────────────────────
               if (ftag == FRAME_REINSTALL_HANDLER)
                  {
                  ctx->handler_stack.push_back(frame.v1);
                  continue;
                  }

               // ── FRAME_NONCONTIN_RETURN ────────────────────────────────
               if (ftag == FRAME_NONCONTIN_RETURN)
                  {
                  throw SchemeRaised(
                      make_error_object("exception handler returned", {frame.v1}),
                      nullptr, false);
                  }

               // ── FRAME_MAKE_PARAMETER ──────────────────────────────────
               if (ftag == FRAME_MAKE_PARAMETER)
                  {
                  V = make_parameter(V, frame.v1);
                  continue;
                  }

               // ── FRAME_FORCE_RESULT ────────────────────────────────────
               if (ftag == FRAME_FORCE_RESULT)
                  {
                  Value p = frame.v1;
                  // Only delay-force promises tail-chase into a promise
                  // result; plain delay resolves to it as-is (R7RS 4.2.5).
                  if (as_promise_is_iterative(p) && is_promise(V))
                     {
                     promise_become(p, V);
                     if (as_promise_is_done(p))
                        {
                        V = as_promise_payload(p);
                        continue;
                        }
                     Frame nf;
                     nf.tag = FRAME_FORCE_RESULT;
                     nf.v1 = p;
                     K.push_back(std::move(nf));
                     Value thunk = as_promise_payload(p);
                     std::vector<Value> ea;
                     EnterResult result = enter_proc(thunk, ea, ctx, E, nullptr);
                     if (apply_enter_result(result, K, V, C, E) == EnterAction::DoApply)
                        continue;
                     break;
                     }
                  promise_resolve(p, V);
                  continue;
                  }

               // ── FRAME_ARG ─────────────────────────────────────────────
               if (ftag == FRAME_ARG)
                  {
                  auto& args = frame.list1;
                  Environment* saved_env = frame.env;
                  Value app_node = frame.v1;

                  if (args.empty())
                     {
                     // V is the fn; 0 args
                     if (is_continuation(V))
                        {
                        K.push_back(make_wind_step_frame(
                            ctx, as_continuation_wind(V), V, continuation_value({})));
                        continue;
                        }
                     auto pv = apply_parameter_if(V, 0, &app_node);
                     if (pv.has_value())
                        {
                        V = *pv;
                        continue;
                        }

                     // Tracer
                     bool trc_printed = false;
                     std::string trc_name;
                     int trc_depth = 0;
                     if (ctx->_instrumented && ctx->tracer && ctx->tracer->_active)
                        {
                        Tracer* trc = ctx->tracer;
                        if (is_cons(app_node) && is_symbol(car(app_node)))
                           trc_name = as_symbol(car(app_node));
                        if (trc_name.empty() && is_primitive(V))
                           trc_name = as_primitive_name(V);
                        if (!trc_name.empty() && trc->_fns_to_trace.count(trc_name))
                           {
                           trc_depth = trc->_depth;
                           trc_printed = trc->trace_enter(trc_name, {}, trc_depth, ctx->outStrm);
                           if (trc_printed)
                              trc->_depth = trc_depth + 1;
                           }
                        }
                     if (is_primitive(V))
                        {
                        if (as_primitive_kind(V) == PRIM_CONTINUATION_DEPTH)
                           {
                           // Internal probe: report the live continuation-stack
                           // (K) length so tail-call tests can assert bounded
                           // continuation space.  K is in scope here but not in
                           // a normal primitive body.
                           V = make_integer((int64_t)K.size());
                           continue;
                           }
                        std::vector<Value> ea;
                        Value result = as_primitive_fn(V)(ctx, saved_env, ea, &app_node);
                        if (trc_printed)
                           {
                           ctx->tracer->_depth = trc_depth;
                           ctx->tracer->trace_exit(trc_name, result, trc_depth, ctx->outStrm);
                           }
                        V = result;
                        continue;
                        }
                     BetaResult r = apply_value(V, {}, &app_node);
                     shadow_push(ctx, K, &app_node);
                     if (trc_printed)
                        {
                        Frame tf;
                        tf.tag = FRAME_TRACE_EXIT;
                        tf.str1 = trc_name;
                        tf.depth = trc_depth;
                        K.push_back(std::move(tf));
                        }
                     E = r.new_env;
                     C = car(r.body);
                     if (is_cons(cdr(r.body)))
                        {
                        Frame sf;
                        sf.tag = FRAME_SEQ;
                        sf.v1 = cdr(r.body);
                        sf.env = r.new_env;
                        K.push_back(std::move(sf));
                        }
                     break;
                     }
                  // Push FRAME_CALL baton; evaluate first arg
                  std::vector<Value> remaining(args.begin() + 1, args.end());
                  Frame cf;
                  cf.tag = FRAME_CALL;
                  cf.v1 = V;
                  cf.list1 = {};
                  cf.list2 = std::move(remaining);
                  cf.env = saved_env;
                  cf.v2 = app_node;
                  K.push_back(std::move(cf));
                  C = args[0];
                  E = saved_env;
                  break;
                  }

               // ── FRAME_CALL ────────────────────────────────────────────
               if (ftag == FRAME_CALL)
                  {
                  Value fn_value = frame.v1;
                  auto& collected = frame.list1;
                  auto& remaining = frame.list2;
                  Environment* saved_env = frame.env;
                  Value app_node = frame.v2;
                  Value original_fn = fn_value;

                  std::vector<Value> new_collected = collected;
                  new_collected.push_back(V);

                  if (!remaining.empty())
                     {
                     // More args to evaluate
                     std::vector<Value> new_remaining(remaining.begin() + 1, remaining.end());
                     Frame nf;
                     nf.tag = FRAME_CALL;
                     nf.v1 = fn_value;
                     nf.list1 = std::move(new_collected);
                     nf.list2 = std::move(new_remaining);
                     nf.env = saved_env;
                     nf.v2 = app_node;
                     K.push_back(std::move(nf));
                     C = remaining[0];
                     E = saved_env;
                     break;
                     }

                  // All args collected; invoke.
                  if (is_continuation(fn_value))
                     {
                     // Drive the wind walk on the K stack via FRAME_WIND_STEP,
                     // which then restores the handler / shadow stacks, swaps in
                     // the continuation's K, and delivers its value -- entirely on
                     // the one loop (no re-entrant cek_eval / no escape throw).
                     K.push_back(make_wind_step_frame(
                         ctx, as_continuation_wind(fn_value), fn_value,
                         continuation_value(new_collected)));
                     continue;
                     }
                  // #2: classify the operator once, then dispatch on the
                  // integer kind below instead of ~15 is_X_prim name
                  // comparisons.  Ordinary primitives and closures get
                  // PRIM_ORDINARY and fall straight through.  Cases that
                  // rewrite fn_value re-classify it so a later case can
                  // catch the new operator, preserving the original
                  // top-to-bottom fall-through semantics.
                  int kind = is_primitive(fn_value)
                             ? as_primitive_kind(fn_value) : PRIM_ORDINARY;
                  // call/cc
                  if (kind == PRIM_CALL_CC)
                     {
                     if (new_collected.size() != 1)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeArityError(
                            arity_mismatch_msg(as_primitive_name(fn_value), 1, 1,
                                               (int)new_collected.size()),
                            src);
                        }
                     // Encode shadow_stack → vector<Value>
                     std::vector<Value> shadow_enc;
                     for (const ShadowEntry& se : ctx->shadow_stack)
                        shadow_enc.push_back(alloc_cons(make_symbol(se.label), make_integer(se.count)));
                     KStack* k_copy = new KStack(K);
                     Value cont = make_continuation(
                         k_copy,
                         std::vector<WindFrame>(ctx->wind_stack),
                         std::vector<Value>(ctx->handler_stack),
                         std::move(shadow_enc));
                     fn_value = new_collected[0];
                     new_collected = {cont};
                     kind = is_primitive(fn_value)
                            ? as_primitive_kind(fn_value) : PRIM_ORDINARY;
                     }
                  // apply (loop in case of (apply apply ...))
                  while (kind == PRIM_APPLY)
                     {
                     auto [proc, flat] = unpack_apply_args(new_collected, &app_node);
                     if (!is_primitive(proc) && !is_closure(proc) &&
                         !is_case_closure(proc) && !is_continuation(proc) &&
                         !is_parameter(proc) && !is_record_accessor(proc) &&
                         !is_record_mutator(proc))
                        {
                        throw SchemeTypeError("apply: first argument must be a procedure", src_of(app_node));
                        }
                     fn_value = proc;
                     new_collected = std::move(flat);
                     kind = is_primitive(fn_value)
                            ? as_primitive_kind(fn_value) : PRIM_ORDINARY;
                     }
                  // map / for-each / filter, the vector/string variants, and 3-arg
                  // member / assoc: drive the per-element calls on the K stack
                  // (FRAME_HOF_STEP / _IDX / FRAME_SEARCH_STEP) instead of the
                  // _prim_* loop, which re-enters cek_eval per element and grows the
                  // C stack when such calls nest.  build_hof_frame is the single
                  // source of truth, shared with enter_proc so the same primitive
                  // reached as a callback is driven on frames too -- no _prim_*
                  // re-entry on any path.  Reached for both operator position and
                  // (apply map ...).
                  {
                  auto hof = build_hof_frame(fn_value, kind, new_collected, &app_node);
                  if (hof.has_value())
                     {
                     K.push_back(std::move(*hof));
                     continue;
                     }
                  }
                  // call-with-values
                  if (kind == PRIM_CALL_WITH_VALUES)
                     {
                     if (new_collected.size() != 2)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeArityError(
                            arity_mismatch_msg("call-with-values", 2, 2, (int)new_collected.size()), src);
                        }
                     Value producer = new_collected[0];
                     Value consumer = new_collected[1];
                     Frame cf;
                     cf.tag = FRAME_CWV_CONSUMER;
                     cf.v1 = consumer;
                     cf.v2 = app_node;
                     K.push_back(std::move(cf));
                     fn_value = producer;
                     new_collected = {};
                     kind = is_primitive(fn_value)
                            ? as_primitive_kind(fn_value) : PRIM_ORDINARY;
                     }
                  // force
                  if (kind == PRIM_FORCE)
                     {
                     if (new_collected.size() != 1)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeArityError(
                            arity_mismatch_msg("force", 1, 1, (int)new_collected.size()), src);
                        }
                     Value p = new_collected[0];
                     if (!is_promise(p))
                        {
                        V = p;
                        continue;
                        }
                     if (as_promise_is_done(p))
                        {
                        V = as_promise_payload(p);
                        continue;
                        }
                     Frame ff;
                     ff.tag = FRAME_FORCE_RESULT;
                     ff.v1 = p;
                     K.push_back(std::move(ff));
                     fn_value = as_promise_payload(p);
                     new_collected = {};
                     kind = is_primitive(fn_value)
                            ? as_primitive_kind(fn_value) : PRIM_ORDINARY;
                     }
                  // make-parameter
                  if (kind == PRIM_MAKE_PARAMETER)
                     {
                     int n = (int)new_collected.size();
                     if (n < 1 || n > 2)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeArityError(
                            arity_mismatch_msg("make-parameter", 1, 2, n), src);
                        }
                     if (n == 1)
                        {
                        V = make_parameter(new_collected[0], NIL_VALUE);
                        continue;
                        }
                     Value converter = new_collected[1];
                     Value init = new_collected[0];
                     if (!is_primitive(converter) && !is_closure(converter) && !is_case_closure(converter))
                        {
                        throw SchemeTypeError("make-parameter: converter must be a procedure", src_of(app_node));
                        }
                     Frame mf;
                     mf.tag = FRAME_MAKE_PARAMETER;
                     mf.v1 = converter;
                     K.push_back(std::move(mf));
                     fn_value = converter;
                     new_collected = {init};
                     kind = is_primitive(fn_value)
                            ? as_primitive_kind(fn_value) : PRIM_ORDINARY;
                     }
                  // with-exception-handler
                  if (kind == PRIM_WITH_EXCEPTION_HANDLER)
                     {
                     if (new_collected.size() != 2)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeArityError(
                            arity_mismatch_msg("with-exception-handler", 2, 2, (int)new_collected.size()), src);
                        }
                     Value handler = new_collected[0];
                     Value thunk = new_collected[1];
                     ctx->handler_stack.push_back(handler);
                     Frame hf;
                     hf.tag = FRAME_POP_HANDLER;
                     K.push_back(std::move(hf));
                     fn_value = thunk;
                     new_collected = {};
                     kind = is_primitive(fn_value)
                            ? as_primitive_kind(fn_value) : PRIM_ORDINARY;
                     }
                  // %guard-eval: guard body evaluator.  Uses FRAME_GUARD
                  // so tail-call replacement only fires within the same
                  // guard form (body pointer identity), not across
                  // weh/guard boundaries.  Guard handlers may return
                  // normally so FRAME_NONCONTIN_RETURN is not pushed.
                  if (kind == PRIM_GUARD_EVAL)
                     {
                     if (new_collected.size() != 2)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeArityError(
                            arity_mismatch_msg("%guard-eval", 2, 2, (int)new_collected.size()), src);
                        }
                     Value handler = new_collected[0];
                     Value thunk = new_collected[1];
                        // Replace previous FRAME_GUARD only when this is
                        // the same guard form (tail-recursive loop).  Same
                        // parsed lambda body means same guard, not nested.
                        // Skip past FRAME_SHADOW_POP frames to find the real top.
                        {
                        int _gi = (int)K.size() - 1;
                        while (_gi >= 0 && K[_gi].tag == FRAME_SHADOW_POP)
                           --_gi;
                        if (_gi >= 0 && K[_gi].tag == FRAME_GUARD &&
                            !ctx->handler_stack.empty())
                           {
                           const Value& prev = ctx->handler_stack.back();
                           if (is_closure(prev) && is_closure(handler))
                              {
                              const Value& pb = std::get<SchemeClosure*>(prev.repr)->body;
                              const Value& hb = std::get<SchemeClosure*>(handler.repr)->body;
                              if (std::holds_alternative<ConsCell*>(pb.repr) &&
                                  std::holds_alternative<ConsCell*>(hb.repr) &&
                                  std::get<ConsCell*>(pb.repr) == std::get<ConsCell*>(hb.repr))
                                 {
                                 // Pop shadow frames above FRAME_GUARD, then FRAME_GUARD itself
                                 while ((int)K.size() - 1 > _gi)
                                    {
                                    if (!ctx->shadow_stack.empty())
                                       ctx->shadow_stack.pop_back();
                                    K.pop_back();
                                    }
                                 K.pop_back();
                                 ctx->handler_stack.pop_back();
                                 }
                              }
                           }
                        }
                     ctx->handler_stack.push_back(handler);
                     Frame gf;
                     gf.tag = FRAME_GUARD;
                     K.push_back(std::move(gf));
                     fn_value = thunk;
                     new_collected = {};
                     kind = is_primitive(fn_value)
                            ? as_primitive_kind(fn_value) : PRIM_ORDINARY;
                     }
                  // raise
                  if (kind == PRIM_RAISE)
                     {
                     if (new_collected.size() != 1)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeArityError(
                            arity_mismatch_msg("raise", 1, 1, (int)new_collected.size()), src);
                        }
                     throw SchemeRaised(new_collected[0], src_of(app_node), false);
                     }
                  // raise-continuable
                  if (kind == PRIM_RAISE_CONTINUABLE)
                     {
                     if (new_collected.size() != 1)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeArityError(
                            arity_mismatch_msg("raise-continuable", 1, 1, (int)new_collected.size()), src);
                        }
                     Value raised_val = new_collected[0];
                     if (ctx->handler_stack.empty())
                        throw SchemeRaised(raised_val, src_of(app_node), true);
                     Value handler = ctx->handler_stack.back();
                     ctx->handler_stack.pop_back();
                     Frame rf;
                     rf.tag = FRAME_REINSTALL_HANDLER;
                     rf.v1 = handler;
                     K.push_back(std::move(rf));
                     fn_value = handler;
                     new_collected = {raised_val};
                     kind = is_primitive(fn_value)
                            ? as_primitive_kind(fn_value) : PRIM_ORDINARY;
                     }
                  // eval
                  if (kind == PRIM_EVAL)
                     {
                     int n = (int)new_collected.size();
                     if (n < 1 || n > 2)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeArityError(arity_mismatch_msg("eval", 1, 2, n), src);
                        }
                     Value datum = new_collected[0];
                     Environment* target_env;
                     if (n == 2)
                        {
                        Value env_arg = new_collected[1];
                        if (!is_environment(env_arg))
                           throw SchemeTypeError("eval: second argument must be an environment",
                                                 src_of(app_node));
                        target_env = as_environment(env_arg);
                        }
                     else
                        {
                        target_env = saved_env->getGlobalEnv();
                        }
                     if (is_nil(datum))
                        throw SchemeAnalysisError(
                            "empty list () is not a valid expression; use (quote ()) for the empty list",
                            src_of(datum));
                     Value expanded = expand(datum);
                     C = expanded;
                     E = target_env;
                     break;
                     }
                  // error
                  if (kind == PRIM_ERROR)
                     {
                     int n = (int)new_collected.size();
                     if (n < 1)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeArityError(arity_mismatch_msg("error", 1, -1, n), src);
                        }
                     if (!is_string(new_collected[0]))
                        throw SchemeTypeError("error: first argument must be a string", src_of(app_node));
                     std::string msg = as_string(new_collected[0]);
                     std::vector<Value> irritants(new_collected.begin() + 1, new_collected.end());
                     throw SchemeUserError(msg, std::move(irritants), src_of(app_node));
                     }
                  // %with-parameters
                  if (kind == PRIM_WITH_PARAMETERS)
                     {
                     if (new_collected.size() != 3)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeArityError(
                            arity_mismatch_msg("%with-parameters", 3, 3, (int)new_collected.size()), src);
                        }
                        // TCO: if the immediately enclosing wind frame is a
                        // parameterize restore, pop and execute it now so
                        // tail-recursive parameterize loops don't accumulate frames.
                        // Skip past FRAME_SHADOW_POP frames to find the real top.
                        {
                        int _pi = (int)K.size() - 1;
                        while (_pi >= 0 && K[_pi].tag == FRAME_SHADOW_POP)
                           --_pi;
                        if (_pi >= 0 && K[_pi].tag == FRAME_DYNAMIC_WIND_AFTER &&
                            is_parameterize_restore_prim(K[_pi].v1))
                           {
                           Value prev_restore = K[_pi].v1;
                           while ((int)K.size() - 1 > _pi)
                              {
                              if (!ctx->shadow_stack.empty())
                                 ctx->shadow_stack.pop_back();
                              K.pop_back();
                              }
                           K.pop_back();
                           if (!ctx->wind_stack.empty())
                              ctx->wind_stack.pop_back();
                           std::vector<Value> ea;
                           apply_scheme_proc(prev_restore, ea, ctx, saved_env, &app_node);
                           }
                        }
                     // Resolve params/values (pure), then drive the value converters
                     // on the K stack via FRAME_PARAMETERIZE_STEP; its final step
                     // installs the winds (native-closure install/restore) and
                     // tail-calls the body thunk.  No re-entrant apply_scheme_proc.
                     auto pp = resolve_parameterize_params(
                         new_collected[0], new_collected[1], ctx, &app_node);
                     Frame ps;
                     ps.tag = FRAME_PARAMETERIZE_STEP;
                     ps.list1 = std::move(pp.first);   // params
                     ps.list2 = std::move(pp.second);  // raw_vals
                     ps.depth = 0;                     // index
                     ps.uid = 0;                       // awaiting = false
                     ps.v1 = new_collected[2];         // thunk
                     ps.v2 = app_node;                 // app_node
                     K.push_back(std::move(ps));
                     continue;
                     }
                  // dynamic-wind
                  if (kind == PRIM_DYNAMIC_WIND)
                     {
                     if (new_collected.size() != 3)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeArityError(
                            arity_mismatch_msg("dynamic-wind", 3, 3, (int)new_collected.size()), src);
                        }
                     // Run the before-thunk ON THE K STACK (not via a re-entrant
                     // apply_scheme_proc); when it returns,
                     // FRAME_DYNAMIC_WIND_BEFORE_DONE installs the wind +
                     // after-frame and tail-calls the body.  If before raises, no
                     // wind is installed (the frame is discarded during unwind) --
                     // matching the old eager call.  (No GcRootGuards needed: the
                     // before/thunk/after Values ride on the frame, which is
                     // GC-traced, and enter_proc itself does not collect.)
                     Frame bd;
                     bd.tag = FRAME_DYNAMIC_WIND_BEFORE_DONE;
                     bd.v1 = new_collected[0];        // before
                     bd.v2 = new_collected[1];        // thunk
                     bd.list1 = {new_collected[2]};   // {after}
                     K.push_back(std::move(bd));
                     fn_value = new_collected[0];
                     new_collected = {};
                     // kind stays PRIM_DYNAMIC_WIND so the PRIM_PORT_RUNNER block
                     // below does not re-fire; the terminal apply dispatches the
                     // before-thunk fresh on fn_value.
                     }
                  // port runners (call-with-port / call-with-{input,output}-file /
                  // with-{input,output}-{from,to}-{file,string}): open + set up,
                  // then ride the dynamic-wind machinery -- a native after-thunk
                  // (close port; with-* also restore a current-port param) runs on
                  // every exit, and the proc/thunk is tail-called on the K stack.
                  if (kind == PRIM_PORT_RUNNER)
                     {
                     PortRunnerSetup prs = port_runner_setup(
                         as_primitive_name(fn_value), ctx, saved_env,
                         new_collected, &app_node);
                     WindFrame wf;
                     wf.before = prs.before;
                     wf.after = prs.after;
                     ctx->wind_stack.push_back(wf);
                     Frame df;
                     df.tag = FRAME_DYNAMIC_WIND_AFTER;
                     df.v1 = prs.after;
                     K.push_back(std::move(df));
                     fn_value = prs.body_proc;
                     new_collected = std::move(prs.body_args);
                     kind = is_primitive(fn_value)
                            ? as_primitive_kind(fn_value) : PRIM_ORDINARY;
                     }
                  // load: read + parse the file (native), then evaluate its
                  // top-level forms on the K stack via FRAME_EVAL_FORMS instead of
                  // a re-entrant cek_eval per form.  Reached for the operator
                  // position and (apply load ...) alike.
                  if (kind == PRIM_LOAD)
                     {
                     LoadSetup ls = load_setup(new_collected, saved_env, &app_node);
                     Frame ef;
                     ef.tag = FRAME_EVAL_FORMS;
                     ef.list1 = std::move(ls.forms);
                     ef.env = ls.eval_env;
                     ef.depth = 0;
                     K.push_back(std::move(ef));
                     continue;
                     }
                     // parameter?
                     {
                     auto pv = apply_parameter_if(fn_value, (int)new_collected.size(), &app_node);
                     if (pv.has_value())
                        {
                        V = *pv;
                        continue;
                        }
                     }
                  // record-accessor
                  if (is_record_accessor(fn_value))
                     {
                     if (new_collected.size() != 1)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeArityError(
                            arity_mismatch_msg(as_record_accessor_name(fn_value), 1, 1,
                                               (int)new_collected.size()),
                            src);
                        }
                     RecordType* rt = as_record_accessor_type(fn_value);
                     Value rec = new_collected[0];
                     if (!is_record(rec) || as_record_type(rec) != rt)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeTypeError(
                            as_record_accessor_name(fn_value) +
                                ": argument is not a " + rt->name,
                            src);
                        }
                     V = as_record_fields_const(rec)[as_record_accessor_index(fn_value)];
                     continue;
                     }
                  // record-mutator
                  if (is_record_mutator(fn_value))
                     {
                     if (new_collected.size() != 2)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeArityError(
                            arity_mismatch_msg(as_record_mutator_name(fn_value), 2, 2,
                                               (int)new_collected.size()),
                            src);
                        }
                     RecordType* rt = as_record_mutator_type(fn_value);
                     Value rec = new_collected[0];
                     if (!is_record(rec) || as_record_type(rec) != rt)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeTypeError(
                            as_record_mutator_name(fn_value) +
                                ": first argument is not a " + rt->name,
                            src);
                        }
                     as_record_fields(new_collected[0])[as_record_mutator_index(fn_value)] = new_collected[1];
                     V = VOID_VALUE;
                     continue;
                     }
                  // Tracer for closure/primitive call
                  bool trc_printed = false;
                  std::string trc_name;
                  int trc_depth = 0;
                  if (ctx->_instrumented && ctx->tracer && ctx->tracer->_active)
                     {
                     Tracer* trc = ctx->tracer;
                     if (is_cons(app_node) && is_symbol(car(app_node)))
                        trc_name = as_symbol(car(app_node));
                     if (trc_name.empty() && is_primitive(fn_value))
                        trc_name = as_primitive_name(fn_value);
                     if (!trc_name.empty() && trc->_fns_to_trace.count(trc_name))
                        {
                        trc_depth = trc->_depth;
                        trc_printed = trc->trace_enter(trc_name, new_collected, trc_depth, ctx->outStrm);
                        if (trc_printed)
                           trc->_depth = trc_depth + 1;
                        }
                     }
                  if (is_primitive(fn_value))
                     {
                     V = as_primitive_fn(fn_value)(ctx, saved_env, new_collected, &app_node);
                     if (trc_printed)
                        {
                        ctx->tracer->_depth = trc_depth;
                        ctx->tracer->trace_exit(trc_name, V, trc_depth, ctx->outStrm);
                        }
                     continue;
                     }
                  BetaResult r = apply_value(fn_value, new_collected, &app_node);
                  if (val_id(fn_value) == val_id(original_fn))
                     shadow_push(ctx, K, &app_node);
                  if (trc_printed)
                     {
                     Frame tf;
                     tf.tag = FRAME_TRACE_EXIT;
                     tf.str1 = trc_name;
                     tf.depth = trc_depth;
                     K.push_back(std::move(tf));
                     }
                  E = r.new_env;
                  C = car(r.body);
                  if (is_cons(cdr(r.body)))
                     {
                     Frame sf;
                     sf.tag = FRAME_SEQ;
                     sf.v1 = cdr(r.body);
                     sf.env = r.new_env;
                     K.push_back(std::move(sf));
                     }
                  break;
                  }

               // ── FRAME_SEQ ─────────────────────────────────────────────
               if (ftag == FRAME_SEQ)
                  {
                  Value remaining = frame.v1;
                  E = frame.env;
                  C = car(remaining);
                  if (is_cons(cdr(remaining)))
                     {
                     Frame sf;
                     sf.tag = FRAME_SEQ;
                     sf.v1 = cdr(remaining);
                     sf.env = frame.env;
                     K.push_back(std::move(sf));
                     }
                  break;
                  }

               // ── FRAME_WHEN ────────────────────────────────────────────
               if (ftag == FRAME_WHEN)
                  {
                  Value body = frame.v1;
                  E = frame.env;
                  if (is_false_val(V))
                     {
                     V = VOID_VALUE;
                     continue;
                     }
                  C = car(body);
                  if (is_cons(cdr(body)))
                     {
                     Frame sf;
                     sf.tag = FRAME_SEQ;
                     sf.v1 = cdr(body);
                     sf.env = frame.env;
                     K.push_back(std::move(sf));
                     }
                  break;
                  }

               // ── FRAME_UNLESS ──────────────────────────────────────────
               if (ftag == FRAME_UNLESS)
                  {
                  Value body = frame.v1;
                  E = frame.env;
                  if (!is_false_val(V))
                     {
                     V = VOID_VALUE;
                     continue;
                     }
                  C = car(body);
                  if (is_cons(cdr(body)))
                     {
                     Frame sf;
                     sf.tag = FRAME_SEQ;
                     sf.v1 = cdr(body);
                     sf.env = frame.env;
                     K.push_back(std::move(sf));
                     }
                  break;
                  }

               // ── FRAME_AND ─────────────────────────────────────────────
               if (ftag == FRAME_AND)
                  {
                  Value remaining = frame.v1;
                  E = frame.env;
                  if (is_false_val(V))
                     continue;
                  if (is_nil(remaining))
                     continue;
                  if (is_cons(cdr(remaining)))
                     {
                     Frame sf;
                     sf.tag = FRAME_AND;
                     sf.v1 = cdr(remaining);
                     sf.env = frame.env;
                     K.push_back(std::move(sf));
                     }
                  C = car(remaining);
                  break;
                  }

               // ── FRAME_OR ──────────────────────────────────────────────
               if (ftag == FRAME_OR)
                  {
                  Value remaining = frame.v1;
                  E = frame.env;
                  if (!is_false_val(V))
                     continue;
                  if (is_nil(remaining))
                     continue;
                  if (is_cons(cdr(remaining)))
                     {
                     Frame sf;
                     sf.tag = FRAME_OR;
                     sf.v1 = cdr(remaining);
                     sf.env = frame.env;
                     K.push_back(std::move(sf));
                     }
                  C = car(remaining);
                  break;
                  }

               // ── FRAME_COND ────────────────────────────────────────────
               if (ftag == FRAME_COND)
                  {
                  Value current = frame.v1;
                  Value remaining = frame.v2;
                  Environment* saved_env = frame.env;
                  if (is_false_val(V))
                     {
                     if (is_nil(remaining))
                        {
                        V = VOID_VALUE;
                        continue;
                        }
                     Value nxt = car(remaining);
                     Value rest = cdr(remaining);
                     CondClause kind = classify_cond_clause(nxt, saved_env);
                     E = saved_env;
                     if (kind.kind == CondClause::Else)
                        {
                        Value body = kind.body_cons;
                        C = car(body);
                        if (is_cons(cdr(body)))
                           {
                           Frame sf;
                           sf.tag = FRAME_SEQ;
                           sf.v1 = cdr(body);
                           sf.env = saved_env;
                           K.push_back(std::move(sf));
                           }
                        break;
                        }
                     Frame cf;
                     cf.tag = FRAME_COND;
                     cf.v1 = nxt;
                     cf.v2 = rest;
                     cf.env = saved_env;
                     K.push_back(std::move(cf));
                     C = kind.test;
                     break;
                     }
                  // Test truthy
                  CondClause kind = classify_cond_clause(current, saved_env);
                  if (kind.kind == CondClause::TestOnly)
                     continue;
                  if (kind.kind == CondClause::Body)
                     {
                     Value body = kind.body_cons;
                     E = saved_env;
                     C = car(body);
                     if (is_cons(cdr(body)))
                        {
                        Frame sf;
                        sf.tag = FRAME_SEQ;
                        sf.v1 = cdr(body);
                        sf.env = saved_env;
                        K.push_back(std::move(sf));
                        }
                     break;
                     }
                  // Arrow
                  Frame af;
                  af.tag = FRAME_COND_ARROW;
                  af.v1 = V;
                  af.env = saved_env;
                  K.push_back(std::move(af));
                  C = kind.proc_expr;
                  E = saved_env;
                  break;
                  }

               // ── FRAME_COND_ARROW / FRAME_CASE_ARROW ───────────────────
               if (ftag == FRAME_COND_ARROW || ftag == FRAME_CASE_ARROW)
                  {
                  // (cond (test => recv)) / (case key (... => recv)): apply the
                  // receiver to the single value (test / key, in frame.v1)
                  // through the one unified application path (enter_proc), so
                  // primitives, continuations, parameters, record accessors /
                  // mutators, and frame-driven HOFs behave exactly as in operator
                  // position -- rather than the old inline ladder that mishandled
                  // the last two.
                  Value arg_value = frame.v1;
                  Environment* saved_env = frame.env;
                  std::vector<Value> a = {arg_value};
                  EnterResult result = enter_proc(V, a, ctx, saved_env, nullptr);
                  if (apply_enter_result(result, K, V, C, E) == EnterAction::DoApply)
                     continue;
                  break;
                  }

               // ── FRAME_CASE ────────────────────────────────────────────
               if (ftag == FRAME_CASE)
                  {
                  Value current_clause = frame.v1;
                  Value remaining = frame.v2;
                  Environment* saved_env = frame.env;
                  Value head = car(current_clause);
                  if (is_aux_keyword(head, "else", saved_env))
                     {
                     Value body = cdr(current_clause);
                     E = saved_env;
                     if (is_cons(body) && is_symbol(car(body)) && as_symbol(car(body)) == "=>")
                        {
                        Frame af;
                        af.tag = FRAME_CASE_ARROW;
                        af.v1 = V;
                        af.env = saved_env;
                        K.push_back(std::move(af));
                        C = car(cdr(body));
                        }
                     else
                        {
                        C = car(body);
                        if (is_cons(cdr(body)))
                           {
                           Frame sf;
                           sf.tag = FRAME_SEQ;
                           sf.v1 = cdr(body);
                           sf.env = saved_env;
                           K.push_back(std::move(sf));
                           }
                        }
                     break;
                     }
                  // Datum list match
                  bool matched = false;
                  Value cur2 = head;
                  while (is_cons(cur2))
                     {
                     if (eqv_atom(V, car(cur2)))
                        {
                        matched = true;
                        break;
                        }
                     cur2 = cdr(cur2);
                     }
                  if (matched)
                     {
                     Value body = cdr(current_clause);
                     E = saved_env;
                     if (is_cons(body) && is_symbol(car(body)) && as_symbol(car(body)) == "=>")
                        {
                        Frame af;
                        af.tag = FRAME_CASE_ARROW;
                        af.v1 = V;
                        af.env = saved_env;
                        K.push_back(std::move(af));
                        C = car(cdr(body));
                        }
                     else
                        {
                        C = car(body);
                        if (is_cons(cdr(body)))
                           {
                           Frame sf;
                           sf.tag = FRAME_SEQ;
                           sf.v1 = cdr(body);
                           sf.env = saved_env;
                           K.push_back(std::move(sf));
                           }
                        }
                     break;
                     }
                  if (is_nil(remaining))
                     {
                     V = VOID_VALUE;
                     continue;
                     }
                  Frame cf;
                  cf.tag = FRAME_CASE;
                  cf.v1 = car(remaining);
                  cf.v2 = cdr(remaining);
                  cf.env = saved_env;
                  K.push_back(std::move(cf));
                  continue;
                  }

               // ── FRAME_LET ─────────────────────────────────────────────
               if (ftag == FRAME_LET)
                  {
                  auto& names_ids = frame.ids;
                  auto& collected = frame.list1;
                  auto& remaining = frame.list2;
                  Value body = frame.v1;
                  Environment* saved_env = frame.env;
                  std::vector<Value> new_collected = collected;
                  new_collected.push_back(V);
                  if (remaining.empty())
                     {
                     Environment* new_env = gc_alloc_environment(saved_env);
                     for (size_t i = 0; i < names_ids.size(); ++i)
                        new_env->bind_id(names_ids[i], new_collected[i]);
                     E = new_env;
                     C = car(body);
                     if (is_cons(cdr(body)))
                        {
                        Frame sf;
                        sf.tag = FRAME_SEQ;
                        sf.v1 = cdr(body);
                        sf.env = new_env;
                        K.push_back(std::move(sf));
                        }
                     break;
                     }
                  std::vector<Value> new_remaining(remaining.begin() + 1, remaining.end());
                  Frame nf;
                  nf.tag = FRAME_LET;
                  nf.ids = names_ids;
                  nf.list1 = std::move(new_collected);
                  nf.list2 = std::move(new_remaining);
                  nf.v1 = body;
                  nf.env = saved_env;
                  K.push_back(std::move(nf));
                  C = remaining[0];
                  E = saved_env;
                  break;
                  }

               // ── FRAME_LET_STAR ────────────────────────────────────────
               if (ftag == FRAME_LET_STAR)
                  {
                  uint32_t name_id = frame.uid;
                  auto& remaining = frame.pairs;
                  Value body = frame.v1;
                  Environment* saved_env = frame.env;
                  Environment* new_env = gc_alloc_environment(saved_env);
                  new_env->bind_id(name_id, V);
                  if (remaining.empty())
                     {
                     E = new_env;
                     C = car(body);
                     if (is_cons(cdr(body)))
                        {
                        Frame sf;
                        sf.tag = FRAME_SEQ;
                        sf.v1 = cdr(body);
                        sf.env = new_env;
                        K.push_back(std::move(sf));
                        }
                     break;
                     }
                  auto next_pair = remaining[0];
                  std::vector<std::pair<uint32_t, Value>> new_remaining(remaining.begin() + 1, remaining.end());
                  Frame nf;
                  nf.tag = FRAME_LET_STAR;
                  nf.uid = next_pair.first;
                  nf.pairs = std::move(new_remaining);
                  nf.v1 = body;
                  nf.env = new_env;
                  K.push_back(std::move(nf));
                  C = next_pair.second;
                  E = new_env;
                  break;
                  }

               // ── FRAME_LETREC ──────────────────────────────────────────
               if (ftag == FRAME_LETREC)
                  {
                  uint32_t name_id = frame.uid;
                  auto& remaining = frame.pairs;
                  Value body = frame.v1;
                  Environment* saved_env = frame.env;
                  saved_env->set_id(name_id, V);
                  if (remaining.empty())
                     {
                     E = saved_env;
                     C = car(body);
                     if (is_cons(cdr(body)))
                        {
                        Frame sf;
                        sf.tag = FRAME_SEQ;
                        sf.v1 = cdr(body);
                        sf.env = saved_env;
                        K.push_back(std::move(sf));
                        }
                     break;
                     }
                  auto next_pair = remaining[0];
                  std::vector<std::pair<uint32_t, Value>> new_remaining(remaining.begin() + 1, remaining.end());
                  Frame nf;
                  nf.tag = FRAME_LETREC;
                  nf.uid = next_pair.first;
                  nf.pairs = std::move(new_remaining);
                  nf.v1 = body;
                  nf.env = saved_env;
                  K.push_back(std::move(nf));
                  C = next_pair.second;
                  E = saved_env;
                  break;
                  }

               // ── FRAME_SHADOW_POP ──────────────────────────────────────
               if (ftag == FRAME_SHADOW_POP)
                  {
                  if (!ctx->shadow_stack.empty())
                     ctx->shadow_stack.pop_back();
                  continue;
                  }

               // ── FRAME_TRACE_EXIT ──────────────────────────────────────
               if (ftag == FRAME_TRACE_EXIT)
                  {
                  Tracer* trc = ctx->tracer;
                  trc->_depth = frame.depth;
                  trc->trace_exit(frame.str1, V, frame.depth, ctx->outStrm);
                  continue;
                  }

               throw std::runtime_error("cek_loop: unknown frame tag " + std::to_string(ftag));
               } // end APPLY loop D

            } // end B
         }
      catch (SchemeRaised& e)
         {
         if (!dispatch_exc(e.value, true, e.continuable))
            throw;
         }
      catch (SchemeSyntaxError& e)
         {
         if (!dispatch_exc(make_read_error_object(e.msg, {}), false, false))
            throw;
         }
      catch (PositionedSchemeError& e)
         {
         if (!dispatch_exc(make_error_object(e.msg, {}), false, false))
            throw;
         }
      } // end A
   }

// ── cek_eval ──────────────────────────────────────────────────────────────────

Value cek_eval(const Value& expr, Environment* env, Context* ctx)
   {
   Context local_ctx;
   if (!ctx)
      ctx = &local_ctx;

   size_t wind_depth_entry = ctx->wind_stack.size();
   size_t handler_depth_entry = ctx->handler_stack.size();

   auto cleanup = [&](bool set_call_stack, PositionedSchemeError* pse)
   {
      unwind_winds_on_error(ctx, wind_depth_entry);
      while (ctx->handler_stack.size() > handler_depth_entry)
         ctx->handler_stack.pop_back();
      if (set_call_stack && pse && !pse->call_stack && !ctx->shadow_stack.empty())
         pse->call_stack = new std::vector<ShadowEntry>(ctx->shadow_stack);
      ctx->shadow_stack.clear();
   };

   try
      {
      return cek_loop(expr, env, ctx);
      }
   catch (SchemeRaised& e)
      {
      cleanup(true, &e);
      throw;
      }
   catch (SchemeSyntaxError& e)
      {
      cleanup(true, &e);
      throw;
      }
   catch (PositionedSchemeError& e)
      {
      cleanup(true, &e);
      throw;
      }
   catch (...)
      {
      cleanup(false, nullptr);
      throw;
      }
   }

// ── Public API ────────────────────────────────────────────────────────────────

void set_global_env(Environment* env)
   {
   static bool s_registered = false;
   if (!s_registered)
      {
      gc_env_root_push(&g_global_env);
      s_registered = true;
      }
   g_global_env = env;
   }
Environment* get_global_env()
   {
   return g_global_env;
   }

Value sorted_sym_list(const std::unordered_set<std::string>& fns)
   {
   std::vector<std::string> names(fns.begin(), fns.end());
   std::sort(names.begin(), names.end());
   Value result = NIL_VALUE;
   for (int i = (int)names.size() - 1; i >= 0; --i)
      result = alloc_cons(make_symbol(names[i]), result);
   return result;
   }

// ── Continuation destructor ───────────────────────────────────────────────────

Continuation::~Continuation()
   {
   delete static_cast<KStack*>(frames_ptr);
   }

// ── GC hooks ──────────────────────────────────────────────────────────────────

void gc_trace_continuation_frames(Continuation* cont)
   {
   if (!cont->frames_ptr)
      return;
   KStack* k = static_cast<KStack*>(cont->frames_ptr);
   for (Frame& f : *k)
      {
      gc_trace_value(f.v1);
      gc_trace_value(f.v2);
      if (f.env)
         gc_trace_environment(f.env);
      for (Value& v : f.list1)
         gc_trace_value(v);
      for (Value& v : f.list2)
         gc_trace_value(v);
      for (auto& p : f.pairs)
         gc_trace_value(p.second);
      }
   }

void gc_forward_continuation_frames(Continuation* cont)
   {
   if (!cont->frames_ptr)
      return;
   KStack* k = static_cast<KStack*>(cont->frames_ptr);
   for (Frame& f : *k)
      {
      gc_forward_value(f.v1);
      gc_forward_value(f.v2);
      gc_copy_forward_env(f.env);
      for (Value& v : f.list1)
         gc_forward_value(v);
      for (Value& v : f.list2)
         gc_forward_value(v);
      for (auto& p : f.pairs)
         gc_forward_value(p.second);
      }
   }
