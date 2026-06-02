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
      IsCont,
      IsEnter
      } kind = IsValue;
   Value v;
   Environment* new_env = nullptr;
   Value seq; // IsEnter: remaining body cons or NIL_VALUE
   KStack new_k;
   };

// Thrown when a continuation is invoked from a cek_loop invocation other than
// the one that captured it -- i.e. the invocation is nested below the owner on
// the C++ stack, behind native frames (a for-each / map callback, a
// dynamic-wind thunk, etc.).  Replacing K locally would only redirect the
// nested loop and the escape value would be discarded by the native caller, so
// instead we unwind the C++ stack with this exception until we reach the
// owning loop, which installs the captured continuation.  `cont` carries the
// continuation object (re-rooted at the catch site); `args` are the values
// passed to it.  Deliberately NOT derived from any Scheme error type so the
// handler/guard machinery never treats an escape as a raised condition.
struct ContinuationEscape
   {
   uint64_t owner_eval_id;
   Value cont;
   std::vector<Value> args;
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
static void wind_walk(Context* ctx, const std::vector<WindFrame>& target);
static void unwind_winds_on_error(Context* ctx, size_t target_depth);
static std::pair<Value, Value> build_parameterize_winds(
    const Value& params_list, const Value& values_list,
    Context* ctx, Environment* saved_env, const Value* app_node);
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

static inline bool is_call_cc_prim(const Value& V)
   {
   if (!is_primitive(V))
      return false;
   const std::string& n = as_primitive_name(V);
   return n == "call-with-current-continuation" || n == "call/cc";
   }
static inline bool is_dynamic_wind_prim(const Value& V)
   {
   return is_primitive(V) && as_primitive_name(V) == "dynamic-wind";
   }
static inline bool is_apply_prim(const Value& V)
   {
   return is_primitive(V) && as_primitive_name(V) == "apply";
   }
static inline bool is_cwv_prim(const Value& V)
   {
   return is_primitive(V) && as_primitive_name(V) == "call-with-values";
   }
static inline bool is_force_prim(const Value& V)
   {
   return is_primitive(V) && as_primitive_name(V) == "force";
   }
static inline bool is_with_params_prim(const Value& V)
   {
   return is_primitive(V) && as_primitive_name(V) == "%with-parameters";
   }
static inline bool is_parameterize_restore_prim(const Value& V)
   {
   return is_primitive(V) && as_primitive_name(V) == "%parameterize-restore";
   }
static inline bool is_make_param_prim(const Value& V)
   {
   return is_primitive(V) && as_primitive_name(V) == "make-parameter";
   }
static inline bool is_weh_prim(const Value& V)
   {
   return is_primitive(V) && as_primitive_name(V) == "with-exception-handler";
   }
static inline bool is_raise_prim(const Value& V)
   {
   return is_primitive(V) && as_primitive_name(V) == "raise";
   }
static inline bool is_raise_cont_prim(const Value& V)
   {
   return is_primitive(V) && as_primitive_name(V) == "raise-continuable";
   }
static inline bool is_error_prim(const Value& V)
   {
   return is_primitive(V) && as_primitive_name(V) == "error";
   }
static inline bool is_eval_prim(const Value& V)
   {
   return is_primitive(V) && as_primitive_name(V) == "eval";
   }
static inline bool is_guard_eval_prim(const Value& V)
   {
   return is_primitive(V) && as_primitive_name(V) == "%guard-eval";
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

// True when `id` belongs to a cek_loop still live on the C++ stack.  Used to
// decide whether invoking a continuation owned by `id` must unwind to that loop
// (escape) or may be installed in place because the loop has already returned
// (re-entry, e.g. a continuation saved at top level and invoked from a later
// REPL form).  The stack is shallow, so a linear scan is fine.
static bool eval_id_active(Context* ctx, uint64_t id)
   {
   for (uint64_t e : ctx->eval_id_stack)
      if (e == id)
         return true;
   return false;
   }

// A continuation must escape (throw to unwind native frames) only when its
// owning loop is a still-live ancestor other than the current one.  When the
// owner is the current loop, or has already returned, install it in place.
static bool continuation_must_escape(Context* ctx, const Value& cont, uint64_t my_eval_id)
   {
   uint64_t owner = as_continuation_owner(cont);
   return owner != my_eval_id && eval_id_active(ctx, owner);
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

// ── value identity pointer (for wind_walk) ────────────────────────────────────

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

// ── wind_walk ─────────────────────────────────────────────────────────────────

static void wind_walk(Context* ctx, const std::vector<WindFrame>& target)
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
   while (ws.size() > common)
      {
      Value after = ws.back().after;
      ws.pop_back();
      std::vector<Value> ea;
      apply_scheme_proc(after, ea, ctx, nullptr, nullptr);
      }
   for (size_t i = common; i < target.size(); ++i)
      {
      ws.push_back(target[i]);
      std::vector<Value> ea;
      apply_scheme_proc(target[i].before, ea, ctx, nullptr, nullptr);
      }
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

// ── enter_proc ────────────────────────────────────────────────────────────────

static EnterResult enter_proc(const Value& fn_value, std::vector<Value>& args,
                              Context* ctx, Environment* saved_env, const Value* app_node)
   {
   if (is_continuation(fn_value))
      {
      // Invoked below a still-live owning loop (behind native frames)?  Unwind.
      if (continuation_must_escape(ctx, fn_value, ctx->current_eval_id))
         throw ContinuationEscape{as_continuation_owner(fn_value), fn_value, args};
      wind_walk(ctx, as_continuation_wind(fn_value));
      restore_handler_stack(ctx, as_continuation_handlers(fn_value));
      KStack new_k = *static_cast<const KStack*>(as_continuation_frames(fn_value));
      Value v = continuation_value(args);
      EnterResult r;
      r.kind = EnterResult::IsCont;
      r.v = v;
      r.new_k = std::move(new_k);
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
   if (is_primitive(fn_value))
      {
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
   throw SchemeTypeError("expected a procedure", src);
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
      return true;
   default:
      return false;
      }
   }

// ── build_parameterize_winds ──────────────────────────────────────────────────

static std::pair<Value, Value> build_parameterize_winds(
    const Value& params_list, const Value& values_list,
    Context* ctx, Environment* saved_env, const Value* app_node)
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
   // Rooted: new_vals_raw and installed (below) hold user values across the
   // converter's apply_scheme_proc re-entry, which can trigger a moving minor
   // GC.  A fresh nursery value (e.g. a converter or parameterize value that is
   // a young cons) would otherwise be left stale.  (params holds Parameter
   // objects, which are promoted in place and need no root.)
   GcRootVec new_vals_raw_root(new_vals_raw);
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

   std::vector<Value> installed;
   GcRootVec installed_root(installed);
   for (size_t i = 0; i < params.size(); ++i)
      {
      Value conv = as_parameter_converter(params[i]);
      if (is_nil(conv))
         {
         installed.push_back(new_vals_raw[i]);
         }
      else
         {
         std::vector<Value> ca = {new_vals_raw[i]};
         installed.push_back(apply_scheme_proc(conv, ca, ctx, saved_env, app_node));
         }
      }
   std::vector<Value> saved_vals;
   for (size_t i = 0; i < params.size(); ++i)
      saved_vals.push_back(as_parameter_value(params[i]));
   for (size_t i = 0; i < params.size(); ++i)
      set_parameter_value(params[i], installed[i]);

   auto inst_fn = [params, installed](Context*, Environment*, std::vector<Value>&, const Value*) -> Value
   {
      for (size_t j = 0; j < params.size(); ++j)
         set_parameter_value(const_cast<Value&>(params[j]), installed[j]);
      return VOID_VALUE;
   };
   auto rest_fn = [params, saved_vals](Context*, Environment*, std::vector<Value>&, const Value*) -> Value
   {
      for (size_t j = 0; j < params.size(); ++j)
         set_parameter_value(const_cast<Value&>(params[j]), saved_vals[j]);
      return VOID_VALUE;
   };
   return {make_primitive("%parameterize-install", inst_fn),
           make_primitive("%parameterize-restore", rest_fn)};
   }

// ── _library_load_path ────────────────────────────────────────────────────────
// Port of Evaluator.py _library_load_path.

static std::vector<std::string> _library_load_path()
   {
   std::vector<std::string> parts = {"."};
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

// ── _try_load_library_file ────────────────────────────────────────────────────
// Port of Evaluator.py _try_load_library_file.
// C++ omits .py extension loading (Python-specific); only handles .sld files.

static bool _try_load_library_file(const Value& name_sexpr, Context* ctx)
   {
   std::string key;
   try
      {
      key = library_name_to_key(name_sexpr);
      }
   catch (const std::exception&)
      {
      return false;
      }
   if (library_registered_p(key))
      return true;

   // Build file base path from key: "scheme.base" -> "scheme/base"
   std::string base_path;
   size_t start = 0;
   while (true)
      {
      size_t dot = key.find('.', start);
      std::string part = key.substr(start,
                                    dot == std::string::npos ? dot : dot - start);
      if (!base_path.empty())
         base_path += '/';
      base_path += part;
      if (dot == std::string::npos)
         break;
      start = dot + 1;
      }

   for (const auto& base : _library_load_path())
      {
      std::string prefix = base.empty() ? base_path : (base + "/" + base_path);
      std::string sld_path = prefix + ".sld";

      std::ifstream sld_file(sld_path);
      if (!sld_file.is_open())
         continue;

      std::string source((std::istreambuf_iterator<char>(sld_file)),
                         std::istreambuf_iterator<char>());
      sld_file.close();

      std::vector<Value> forms = scheme_parse(source, sld_path);
      Environment* fresh_env = gc_alloc_environment(nullptr);
      gc_env_root_push(&fresh_env);
      size_t i = 0;
      while (i < forms.size())
         {
         cek_eval(expand(forms[i]), fresh_env, ctx);
         i = i + 1;
         }
      gc_env_root_pop(&fresh_env);

      if (library_registered_p(key))
         return true;
      }
   return library_registered_p(key);
   }

// ── _process_one_lib_decl ─────────────────────────────────────────────────────
// Port of Evaluator.py _process_one_lib_decl.
// Forward declaration needed because cond-expand recurses into itself.

static void _process_one_lib_decl(
    const Value& decl, Environment* lib_env,
    std::vector<std::pair<std::string, std::string>>& export_names,
    Context* ctx);

static void _process_one_lib_decl(
    const Value& decl, Environment* lib_env,
    std::vector<std::pair<std::string, std::string>>& export_names,
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
         Value import_set = car(sets);
         sets = cdr(sets);
         std::unordered_map<std::string, Value> bindings;
         try
            {
            bindings = resolve_import_set(import_set);
            }
         catch (const std::runtime_error& e)
            {
            bool loaded = false;
            if (is_cons(import_set))
               loaded = _try_load_library_file(import_set, ctx);
            if (loaded)
               {
               try
                  {
                  bindings = resolve_import_set(import_set);
                  }
               catch (const std::runtime_error& e2)
                  {
                  throw SchemeSyntaxError(
                      std::string("define-library: import: ") + e2.what(),
                      src_of(import_set));
                  }
               }
            else
               {
               throw SchemeSyntaxError(
                   std::string("define-library: import: ") + e.what(),
                   src_of(import_set));
               }
            }
         for (const auto& [n, val] : bindings)
            lib_env->bind(n, val);
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
      // Root the remaining body forms: evaluating one can GC and would
      // otherwise leave the unprocessed tail dangling.
      GcRootGuard forms_root(forms);
      while (is_cons(forms))
         {
         cek_eval(expand(car(forms)), lib_env, ctx);
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
            _process_one_lib_decl(inner, lib_env, export_names, ctx);
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
                   car(cur_inner), lib_env, export_names, ctx);
               cur_inner = cdr(cur_inner);
               }
            return;
            }
         }
      // No clause matched: silently produce no declarations (R7RS).
      return;
      }

   // Unknown declaration keyword: expand and evaluate in lib_env.
   cek_eval(expand(decl), lib_env, ctx);
   }

// ── process_import ────────────────────────────────────────────────────────────
// Port of Evaluator.py _process_import.

void process_import(const Value& sets_cons, Environment* env, Context* ctx)
   {
   Value cur = sets_cons;
   while (is_cons(cur))
      {
      Value import_set = car(cur);
      cur = cdr(cur);
      std::unordered_map<std::string, Value> bindings;
      try
         {
         bindings = resolve_import_set(import_set);
         }
      catch (const std::runtime_error& e)
         {
         bool loaded = false;
         if (is_cons(import_set) && ctx != nullptr)
            loaded = _try_load_library_file(import_set, ctx);
         if (loaded)
            {
            try
               {
               bindings = resolve_import_set(import_set);
               }
            catch (const std::runtime_error& e2)
               {
               throw SchemeSyntaxError(
                   std::string("import: ") + e2.what(),
                   src_of(import_set));
               }
            }
         else
            {
            throw SchemeSyntaxError(
                std::string("import: ") + e.what(),
                src_of(import_set));
            }
         }
      for (const auto& [n, val] : bindings)
         env->bind(n, val);
      }
   }

// ── process_define_library ────────────────────────────────────────────────────
// Port of Evaluator.py _process_define_library.

static void process_define_library(const Value& C, Context* ctx)
   {
   if (!is_cons(cdr(C)))
      throw SchemeSyntaxError("define-library: missing library name", src_of(C));
   Value name_sexpr = car(cdr(C));
   Value decls_cons = cdr(cdr(C));

   std::string key;
   try
      {
      key = library_name_to_key(name_sexpr);
      }
   catch (const std::runtime_error& e)
      {
      throw SchemeSyntaxError(
          std::string("define-library: ") + e.what(), src_of(C));
      }

   Environment* lib_env = gc_alloc_environment(nullptr);
   gc_env_root_push(&lib_env);
   std::vector<std::pair<std::string, std::string>> export_names;

   // Swap runtime env so define-syntax inside library's begin binds into lib_env.
   Environment* outer_env = get_runtime_env();
   set_runtime_env(lib_env);
   try
      {
      Value d = decls_cons;
      while (is_cons(d))
         {
         _process_one_lib_decl(car(d), lib_env, export_names, ctx);
         d = cdr(d);
         }
      }
   catch (...)
      {
      set_runtime_env(outer_env);
      gc_env_root_pop(&lib_env);
      throw;
      }
   set_runtime_env(outer_env);

   // Build exports env: copy each (internal, external) entry out of lib_env.
   Environment* exports_env = gc_alloc_environment(nullptr);
   gc_env_root_push(&exports_env);
   size_t i = 0;
   while (i < export_names.size())
      {
      const std::string& internal_name = export_names[i].first;
      const std::string& external_name = export_names[i].second;
      if (lib_env->_bindings.count(intern_symbol(internal_name)) == 0)
         {
         gc_env_root_pop(&exports_env);
         gc_env_root_pop(&lib_env);
         throw SchemeSyntaxError(
             "define-library: exported name not defined: " + internal_name,
             src_of(C));
         }
      Value val = lib_env->lookup(internal_name);
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
               {
               exports_env->bind(symbol_name(gs_sid), lib_env->_bindings.at(gs_sid));
               }
            }
         }
      i = i + 1;
      }
   exports_env->freeze();
   library_register(key, exports_env);

   gc_env_root_pop(&exports_env);
   gc_env_root_pop(&lib_env);
   }

// ── The CEK machine ───────────────────────────────────────────────────────────

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
         }
      };
   static const KW kw;

   Value C = expr;
   Value V;
   Environment* E = env;
   KStack K;
   bool skip_eval = false;

   // Claim a unique id for this loop invocation and publish it for the
   // duration.  Continuations captured here are stamped with my_eval_id; a
   // ContinuationEscape thrown by a deeper loop is caught here only when its
   // owner matches.  Push onto eval_id_stack so deeper loops can tell this one
   // is still alive (escape) rather than already returned (re-entry).  All
   // state is restored on every exit (normal or exceptional) as the C++ stack
   // unwinds, so the parent loop's id is reinstated.
   const uint64_t my_eval_id = ++ctx->eval_id_counter;
   const uint64_t saved_eval_id = ctx->current_eval_id;
   ctx->current_eval_id = my_eval_id;
   ctx->eval_id_stack.push_back(my_eval_id);
   struct EvalIdGuard
      {
      Context* c;
      uint64_t prev;
      ~EvalIdGuard()
         {
         c->current_eval_id = prev;
         c->eval_id_stack.pop_back();
         }
      } _eval_id_guard{ctx, saved_eval_id};

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
      // The condition object is held across the dynamic-wind after-thunks run
      // during unwind (apply_scheme_proc below), each of which can trigger a
      // moving minor GC.  Root it so it isn't left stale before it is stored
      // into the FRAME_NONCONTIN_RETURN frame / passed to the handler.
      GcRootGuard raised_root(raised_value);
      Value handler_val;
      bool found = false;
      bool is_guard_handler = false;
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
            if (!ctx->handler_stack.empty())
               {
               handler_val = std::move(ctx->handler_stack.back());
               ctx->handler_stack.pop_back();
               found = true;
               }
            break;
            }
         if (f.tag == FRAME_GUARD)
            {
            if (pending_reinstalls > 0)
               {
               --pending_reinstalls;
               continue;
               }
            if (!ctx->handler_stack.empty())
               {
               handler_val = std::move(ctx->handler_stack.back());
               ctx->handler_stack.pop_back();
               found = true;
               is_guard_handler = true;
               }
            break;
            }
         if (f.tag == FRAME_DYNAMIC_WIND_AFTER)
            {
            if (!ctx->wind_stack.empty())
               ctx->wind_stack.pop_back();
            try
               {
               std::vector<Value> ea;
               apply_scheme_proc(f.v1, ea, ctx, nullptr, nullptr);
               }
            catch (...)
               {
               }
            }
         }
      if (!found)
         return false;
      if (is_scheme_raised && !continuable && !is_guard_handler)
         {
         Frame nf;
         nf.tag = FRAME_NONCONTIN_RETURN;
         nf.v1 = raised_root.val;
         K.push_back(std::move(nf));
         }
      std::vector<Value> hargs = {raised_root.val};
      EnterResult result = enter_proc(handler_val, hargs, ctx, E, nullptr);
      if (result.kind == EnterResult::IsValue)
         {
         V = result.v;
         skip_eval = true;
         }
      else if (result.kind == EnterResult::IsCont)
         {
         K = std::move(result.new_k);
         V = result.v;
         skip_eval = true;
         }
      else
         {
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
         }
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
                     uint32_t sid = as_symbol_id(head);

                     if (sid == kw.quote)
                        {
                        V = car(cdr(C));
                        mark_literal_immutable(V);
                        break;
                        }
                     if (sid == kw.lambda)
                        {
                        V = make_closure_from_lambda(C, E);
                        break;
                        }
                     if (sid == kw.case_lambda)
                        {
                        V = make_case_closure_from_form(C, E);
                        break;
                        }
                     if (sid == kw.delay || sid == kw.delay_force)
                        {
                        Value ex = car(cdr(C));
                        Value body = alloc_cons(ex, NIL_VALUE);
                        Value thunk = make_closure({}, body, E, UINT32_MAX, "");
                        // delay-force tail-chases into a promise result;
                        // plain delay returns its value as-is (R7RS 4.2.5).
                        bool iterative = (sid == kw.delay_force);
                        V = make_promise_lazy(thunk, iterative);
                        break;
                        }
                     if (sid == kw.import_)
                        {
                        process_import(cdr(C), E, ctx);
                        V = VOID_VALUE;
                        break;
                        }
                     if (sid == kw.define_library)
                        {
                        process_define_library(C, ctx);
                        V = VOID_VALUE;
                        break;
                        }
                     if (sid == kw.if_)
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
                     if (sid == kw.define)
                        {
                        Frame f;
                        f.tag = FRAME_DEFINE;
                        f.v1 = car(cdr(C));
                        f.env = E;
                        K.push_back(std::move(f));
                        C = car(cdr(cdr(C)));
                        continue;
                        }
                     if (sid == kw.set_)
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
                     if (sid == kw.begin)
                        {
                        Value body = cdr(C);
                        if (is_nil(body))
                           {
                           V = VOID_VALUE;
                           break;
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
                     if (sid == kw.when)
                        {
                        Frame f;
                        f.tag = FRAME_WHEN;
                        f.v1 = cdr(cdr(C));
                        f.env = E;
                        K.push_back(std::move(f));
                        C = car(cdr(C));
                        continue;
                        }
                     if (sid == kw.unless)
                        {
                        Frame f;
                        f.tag = FRAME_UNLESS;
                        f.v1 = cdr(cdr(C));
                        f.env = E;
                        K.push_back(std::move(f));
                        C = car(cdr(C));
                        continue;
                        }
                     if (sid == kw.and_)
                        {
                        Value body = cdr(C);
                        if (is_nil(body))
                           {
                           V = make_boolean(true);
                           break;
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
                     if (sid == kw.or_)
                        {
                        Value body = cdr(C);
                        if (is_nil(body))
                           {
                           V = make_boolean(false);
                           break;
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
                     if (sid == kw.cond)
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
                     if (sid == kw.case_)
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
                     if (sid == kw.let)
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
                           break;
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
                     if (sid == kw.let_star)
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
                     if (sid == kw.letrec || sid == kw.letrec_star)
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
                     if (sid == kw.trace)
                        {
                        Tracer* trc = ctx->tracer;
                        Value args_cons = cdr(C);
                        if (is_nil(args_cons))
                           {
                           V = sorted_sym_list(trc->get_fns());
                           break;
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
                        break;
                        }
                     if (sid == kw.untrace)
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
                        break;
                        }
                     }
                     // Application (keyword or non-symbol head falls through)
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
                  E->bind_id(as_symbol_id(frame.v1), V);
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
                  catch (SchemeUnboundError& e)
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
                  // Both `after` and the body's result are held across the
                  // after-thunk's re-entry into the evaluator, which can
                  // trigger a moving minor GC.  Root them so the forwarded
                  // pointers are not left stale (mirrors the before-thunk
                  // path's GcRootGuards).
                  GcRootGuard after(frame.v1);
                  GcRootGuard body_result(V);
                  if (!ctx->wind_stack.empty())
                     ctx->wind_stack.pop_back();
                  std::vector<Value> ea;
                  apply_scheme_proc(after.val, ea, ctx, nullptr, nullptr);
                  V = body_result.val;
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
                  if (result.kind == EnterResult::IsValue)
                     {
                     V = result.v;
                     continue;
                     }
                  if (result.kind == EnterResult::IsCont)
                     {
                     K = std::move(result.new_k);
                     V = result.v;
                     continue;
                     }
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
                     if (result.kind == EnterResult::IsValue)
                        {
                        V = result.v;
                        continue;
                        }
                     if (result.kind == EnterResult::IsCont)
                        {
                        K = std::move(result.new_k);
                        V = result.v;
                        continue;
                        }
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
                        if (continuation_must_escape(ctx, V, my_eval_id))
                           throw ContinuationEscape{as_continuation_owner(V), V, {}};
                        wind_walk(ctx, as_continuation_wind(V));
                        restore_handler_stack(ctx, as_continuation_handlers(V));
                        K = *static_cast<const KStack*>(as_continuation_frames(V));
                        restore_shadow_stack(ctx, as_continuation_shadow(V));
                        V = continuation_value({});
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
                     if (continuation_must_escape(ctx, fn_value, my_eval_id))
                        throw ContinuationEscape{as_continuation_owner(fn_value),
                                                 fn_value, new_collected};
                     wind_walk(ctx, as_continuation_wind(fn_value));
                     restore_handler_stack(ctx, as_continuation_handlers(fn_value));
                     K = *static_cast<const KStack*>(as_continuation_frames(fn_value));
                     restore_shadow_stack(ctx, as_continuation_shadow(fn_value));
                     V = continuation_value(new_collected);
                     continue;
                     }
                  // call/cc
                  if (is_call_cc_prim(fn_value))
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
                         std::move(shadow_enc),
                         my_eval_id);
                     fn_value = new_collected[0];
                     new_collected = {cont};
                     }
                  // apply (loop in case of (apply apply ...))
                  while (is_apply_prim(fn_value))
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
                     }
                  // call-with-values
                  if (is_cwv_prim(fn_value))
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
                     }
                  // force
                  if (is_force_prim(fn_value))
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
                     }
                  // make-parameter
                  if (is_make_param_prim(fn_value))
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
                     }
                  // with-exception-handler
                  if (is_weh_prim(fn_value))
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
                     }
                  // %guard-eval: guard body evaluator.  Uses FRAME_GUARD
                  // so tail-call replacement only fires within the same
                  // guard form (body pointer identity), not across
                  // weh/guard boundaries.  Guard handlers may return
                  // normally so FRAME_NONCONTIN_RETURN is not pushed.
                  if (is_guard_eval_prim(fn_value))
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
                     }
                  // raise
                  if (is_raise_prim(fn_value))
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
                  if (is_raise_cont_prim(fn_value))
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
                     }
                  // eval
                  if (is_eval_prim(fn_value))
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
                  if (is_error_prim(fn_value))
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
                  if (is_with_params_prim(fn_value))
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
                     auto [inst, rest] = build_parameterize_winds(
                         new_collected[0], new_collected[1], ctx, saved_env, &app_node);
                     WindFrame wf;
                     wf.before = inst;
                     wf.after = rest;
                     ctx->wind_stack.push_back(wf);
                     Frame df;
                     df.tag = FRAME_DYNAMIC_WIND_AFTER;
                     df.v1 = rest;
                     K.push_back(std::move(df));
                     fn_value = new_collected[2];
                     new_collected = {};
                     }
                  // dynamic-wind
                  if (is_dynamic_wind_prim(fn_value))
                     {
                     if (new_collected.size() != 3)
                        {
                        SourceInfo* src = src_of(app_node);
                        throw SchemeArityError(
                            arity_mismatch_msg("dynamic-wind", 3, 3, (int)new_collected.size()), src);
                        }
                     Value before = new_collected[0];
                     Value thunk = new_collected[1];
                     Value after = new_collected[2];
                     // Root all three across the before-thunk call: it can
                     // trigger GC, and thunk/after are not yet on the wind
                     // stack (after is pushed below).  Without rooting, a
                     // collection during `before` relocates/reclaims them,
                     // leaving stale closures (garbage arity, corrupt body).
                     GcRootGuard rg_before(before), rg_thunk(thunk), rg_after(after);
                     std::vector<Value> ea;
                     apply_scheme_proc(before, ea, ctx, saved_env, &app_node);
                     WindFrame wf;
                     wf.before = before;
                     wf.after = after;
                     ctx->wind_stack.push_back(wf);
                     Frame df;
                     df.tag = FRAME_DYNAMIC_WIND_AFTER;
                     df.v1 = after;
                     K.push_back(std::move(df));
                     fn_value = thunk;
                     new_collected = {};
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

               // ── FRAME_COND_ARROW ──────────────────────────────────────
               if (ftag == FRAME_COND_ARROW)
                  {
                  Value test_value = frame.v1;
                  Environment* saved_env = frame.env;
                  if (is_continuation(V))
                     {
                     if (continuation_must_escape(ctx, V, my_eval_id))
                        throw ContinuationEscape{as_continuation_owner(V), V, {test_value}};
                     wind_walk(ctx, as_continuation_wind(V));
                     restore_handler_stack(ctx, as_continuation_handlers(V));
                     K = *static_cast<const KStack*>(as_continuation_frames(V));
                     V = continuation_value({test_value});
                     continue;
                     }
                  auto pv = apply_parameter_if(V, 1, nullptr);
                  if (pv.has_value())
                     {
                     V = *pv;
                     continue;
                     }
                  if (is_primitive(V))
                     {
                     std::vector<Value> a = {test_value};
                     V = as_primitive_fn(V)(ctx, saved_env, a, nullptr);
                     continue;
                     }
                  BetaResult r = apply_value(V, {test_value}, nullptr);
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

               // ── FRAME_CASE_ARROW ──────────────────────────────────────
               if (ftag == FRAME_CASE_ARROW)
                  {
                  Value key_value = frame.v1;
                  Environment* saved_env = frame.env;
                  if (is_continuation(V))
                     {
                     if (continuation_must_escape(ctx, V, my_eval_id))
                        throw ContinuationEscape{as_continuation_owner(V), V, {key_value}};
                     wind_walk(ctx, as_continuation_wind(V));
                     restore_handler_stack(ctx, as_continuation_handlers(V));
                     K = *static_cast<const KStack*>(as_continuation_frames(V));
                     V = continuation_value({key_value});
                     continue;
                     }
                  auto pv = apply_parameter_if(V, 1, nullptr);
                  if (pv.has_value())
                     {
                     V = *pv;
                     continue;
                     }
                  if (is_primitive(V))
                     {
                     std::vector<Value> a = {key_value};
                     V = as_primitive_fn(V)(ctx, saved_env, a, nullptr);
                     continue;
                     }
                  BetaResult r = apply_value(V, {key_value}, nullptr);
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
      catch (ContinuationEscape& esc)
         {
         // An escape continuation captured by some loop was invoked below us,
         // behind native frames, and unwound the C++ stack to here.  Only the
         // owning loop may install it; otherwise keep unwinding.
         if (esc.owner_eval_id != my_eval_id)
            throw;
         // Re-root the continuation and its arguments: installing winds may
         // run after-thunks that allocate (and trigger GC), and neither esc
         // member is otherwise reachable from a GC root during this catch.
         GcRootGuard cont_root(esc.cont);
         GcRootVec args_root(esc.args);
         const Value& cont = cont_root.val;
         wind_walk(ctx, as_continuation_wind(cont));
         restore_handler_stack(ctx, as_continuation_handlers(cont));
         K = *static_cast<const KStack*>(as_continuation_frames(cont));
         restore_shadow_stack(ctx, as_continuation_shadow(cont));
         V = continuation_value(esc.args);
         skip_eval = true; // V is ready; resume in the APPLY phase
         continue;         // restart loop A
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
