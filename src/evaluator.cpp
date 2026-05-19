#include "evaluator.h"
#include "gc.h"
#include "symbol.h"
#include "expander.h"
#include "analyzer.h"
#include <iostream>
#include <vector>
#include <string>
#include <optional>
#include <algorithm>
#include <cassert>

// ─────────────────────────────────────────────────────────────────────────────
// Frame tag

enum class FTag : uint8_t
   {
   Define=0, Set=1, If=2, Arg=3, Call=4,
   Seq=5, When=6, Unless=7, And=8, Or=9,
   Cond=10, CondArrow=11, Let=12, LetStar=13, Letrec=14,
   Case=15, DynWindAfter=16, CwvConsumer=17, ForceResult=18,
   MakeParam=19, PopHandler=20, ReinstallHandler=21,
   CaseArrow=22, ShadowPop=23, TraceExit=24, NonContinReturn=25,
   };

static bool multi_values_ok_tag(FTag t)
   {
   return t == FTag::CwvConsumer   || t == FTag::Seq
       || t == FTag::DynWindAfter  || t == FTag::PopHandler
       || t == FTag::ReinstallHandler || t == FTag::NonContinReturn
       || t == FTag::ShadowPop     || t == FTag::TraceExit;
   }

// ─────────────────────────────────────────────────────────────────────────────
// Frame struct
//
// Field usage per tag:
//   Define/Set:      sid=name, env
//   If:              a=then, b=else, env
//   Seq/When/Unless: a=remaining_cons, env
//   And/Or:          a=remaining_cons, env
//   Cond:            a=current_clause, b=remaining_cons, env
//   CondArrow:       a=test_value, env
//   Case:            a=current_clause, b=remaining_cons, env
//   CaseArrow:       a=key_value, env
//   Arg:             va=args_list, env, b=app_node
//   Call:            a=fn, va=collected, vb=remaining, env, b=app_node
//   Let:             names=names, va=collected, vb=remaining_exprs, a=body_cons, env
//   LetStar:         sid=current_name, pairs=remaining, a=body_cons, env
//   Letrec:          sid=current_name, pairs=remaining, a=body_cons, env
//   DynWindAfter:    a=after_thunk
//   CwvConsumer:     a=consumer, b=app_node
//   ForceResult:     a=promise
//   MakeParam:       a=converter
//   ReinstallHandler: a=handler
//   CaseArrow:       a=key_value, env
//   TraceExit:       str=fn_name, depth=depth
//   NonContinReturn: a=raised_value

struct Frame
   {
   FTag         tag;
   Value        a;
   Value        b;
   Environment* env   = nullptr;
   uint32_t     sid   = 0;
   std::vector<Value>                     va;
   std::vector<Value>                     vb;
   std::vector<uint32_t>                  names;
   std::vector<std::pair<uint32_t,Value>> pairs;
   std::string  str;
   int          depth = 0;
   };

// ─────────────────────────────────────────────────────────────────────────────
// GC frame tracing helpers

static void trace_frame(Frame& f)
   {
   gc_trace_value(f.a);
   gc_trace_value(f.b);
   if (f.env) gc_trace_environment(f.env);
   for (auto& v : f.va)          gc_trace_value(v);
   for (auto& v : f.vb)          gc_trace_value(v);
   for (auto& kv : f.pairs)      gc_trace_value(kv.second);
   }

static void forward_frame(Frame& f)
   {
   gc_copy_forward_value(f.a);
   gc_copy_forward_value(f.b);
   if (f.env) gc_copy_forward_env(f.env);
   for (auto& v : f.va)          gc_copy_forward_value(v);
   for (auto& v : f.vb)          gc_copy_forward_value(v);
   for (auto& kv : f.pairs)      gc_copy_forward_value(kv.second);
   }

// ─────────────────────────────────────────────────────────────────────────────
// GC hooks required by gc.h — implemented here (replacing evaluator_stubs.cpp)

void gc_trace_continuation_frames(SchemeContinuation* cont)
   {
   if (!cont->frames_ptr) return;
   auto* k = static_cast<std::vector<Frame>*>(cont->frames_ptr);
   for (auto& f : *k) trace_frame(f);
   for (auto& v : cont->arg_stack_snapshot) gc_trace_value(v);
   for (auto& wf : cont->wind_stack)
      {
      gc_trace_value(wf.before);
      gc_trace_value(wf.after);
      }
   }

void gc_forward_continuation_frames(SchemeContinuation* cont)
   {
   if (!cont->frames_ptr) return;
   auto* k = static_cast<std::vector<Frame>*>(cont->frames_ptr);
   for (auto& f : *k) forward_frame(f);
   for (auto& v : cont->arg_stack_snapshot) gc_copy_forward_value(v);
   for (auto& wf : cont->wind_stack)
      {
      gc_copy_forward_value(wf.before);
      gc_copy_forward_value(wf.after);
      }
   }

SchemeContinuation::~SchemeContinuation()
   {
   if (frames_ptr)
      {
      delete static_cast<std::vector<Frame>*>(frames_ptr);
      frames_ptr = nullptr;
      }
   }

// ─────────────────────────────────────────────────────────────────────────────
// Beta result

struct BetaResult { Environment* new_env; Value body; };

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations

static Value _cek_loop(Value expr, Environment* env, CekCtx& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Beta reduction

static BetaResult beta_reduce_core(
   const std::vector<uint32_t>& params, uint32_t rest,
   Value body, Environment* clo_env,
   const std::vector<Value>& args)
   {
   int nf = (int)params.size();
   int na = (int)args.size();
   bool has_rest = (rest != NO_REST_PARAM);
   if (!has_rest) {
      if (na != nf)
         throw SchemeArityError(arity_mismatch_msg("", nf, nf, na));
   } else {
      if (na < nf)
         throw SchemeArityError(arity_mismatch_msg("", nf, -1, na));
   }
   Environment* ne = gc_alloc_environment(clo_env);
   for (int i = 0; i < nf; ++i) ne->bind_id(params[i], args[i]);
   if (has_rest) {
      Value rl = make_nil();
      for (int i = na - 1; i >= nf; --i) {
         ConsCell* c = gc_alloc_cons(); c->car = args[i]; c->cdr = rl;
         rl = make_cons(c);
      }
      ne->bind_id(rest, rl);
   }
   return {ne, body};
   }

static BetaResult beta_reduce_value(Value fn, const std::vector<Value>& args)
   {
   if (is_case_closure(fn))
      {
      SchemeCaseClosure* cc = as_case_closure(fn);
      int na = (int)args.size();
      for (const auto& cl : cc->clauses)
         {
         int nf = (int)cl.params.size();
         bool vr = (cl.rest_param != NO_REST_PARAM);
         if ((!vr && na == nf) || (vr && na >= nf))
            return beta_reduce_core(cl.params, cl.rest_param, cl.body, cc->env, args);
         }
      throw SchemeArityError("case-lambda: no clause matches "
                             + std::to_string(na) + " arguments");
      }
   if (!is_closure(fn))
      throw SchemeTypeError("application of non-procedure: " + value_to_string(fn, true));
   SchemeClosure* cl = as_closure(fn);
   return beta_reduce_core(cl->params, cl->rest_param, cl->body, cl->env, args);
   }

// ─────────────────────────────────────────────────────────────────────────────
// Build closure values from syntax forms

static Value make_closure_from_lambda(Value lam, Environment* env)
   {
   Value ps = as_cons(as_cons(lam)->cdr)->car;
   Value body = as_cons(as_cons(lam)->cdr)->cdr;
   SchemeClosure* cl = gc_alloc_closure();
   cl->env = env;
   if (is_symbol(ps))
      {
      cl->rest_param = as_symbol_id(ps);
      }
   else if (is_nil(ps))
      {
      cl->rest_param = NO_REST_PARAM;
      }
   else
      {
      Value cur = ps;
      while (is_cons(cur)) { cl->params.push_back(as_symbol_id(as_cons(cur)->car)); cur = as_cons(cur)->cdr; }
      cl->rest_param = is_symbol(cur) ? as_symbol_id(cur) : NO_REST_PARAM;
      }
   // Strip leading docstring from body
   if (is_cons(body) && is_cons(as_cons(body)->cdr) && is_string(as_cons(body)->car))
      cl->body = as_cons(body)->cdr;
   else
      cl->body = body;
   return make_closure(cl);
   }

static Value make_case_closure_from_form(Value form, Environment* env)
   {
   SchemeCaseClosure* cc = gc_alloc_case_closure();
   cc->env = env;
   Value cur = as_cons(form)->cdr;
   while (is_cons(cur))
      {
      Value clause = as_cons(cur)->car;
      Value ps     = as_cons(clause)->car;
      Value body   = as_cons(clause)->cdr;
      SchemeCaseClosure::Clause cl;
      if (is_symbol(ps))
         {
         cl.rest_param = as_symbol_id(ps);
         }
      else if (is_nil(ps))
         {
         cl.rest_param = NO_REST_PARAM;
         }
      else
         {
         Value p = ps;
         while (is_cons(p)) { cl.params.push_back(as_symbol_id(as_cons(p)->car)); p = as_cons(p)->cdr; }
         cl.rest_param = is_symbol(p) ? as_symbol_id(p) : NO_REST_PARAM;
         }
      cl.body = body;
      cc->clauses.push_back(std::move(cl));
      cur = as_cons(cur)->cdr;
      }
   return make_case_closure(cc);
   }

// ─────────────────────────────────────────────────────────────────────────────
// Misc helpers

static std::vector<Value> collect_cons(Value cell)
   {
   std::vector<Value> v;
   Value cur = cell;
   while (is_cons(cur)) { v.push_back(as_cons(cur)->car); cur = as_cons(cur)->cdr; }
   return v;
   }

static std::vector<std::pair<uint32_t,Value>> collect_let_bindings(Value cell)
   {
   std::vector<std::pair<uint32_t,Value>> v;
   Value cur = cell;
   while (is_cons(cur))
      {
      Value b = as_cons(cur)->car;
      v.push_back({as_symbol_id(as_cons(b)->car), as_cons(as_cons(b)->cdr)->car});
      cur = as_cons(cur)->cdr;
      }
   return v;
   }

static Value continuation_value(const std::vector<Value>& args)
   {
   if (args.empty()) return make_unspecified();
   if (args.size() == 1) return args[0];
   SchemeMultiValues* mv = gc_alloc_multi_values();
   mv->values = args;
   return make_multi_values_val(mv);
   }

static std::optional<Value> apply_parameter_if(Value v, int na)
   {
   if (!is_parameter(v)) return std::nullopt;
   if (na != 0) throw SchemeArityError(arity_mismatch_msg("parameter", 0, 0, na));
   return as_parameter(v)->current;
   }

static bool is_builtin_named(Value v, const char* name)
   { return is_builtin(v) && as_builtin(v)->name == name; }

static bool sym_is(Value v, const char* name)
   { return is_symbol(v) && symbol_name(as_symbol_id(v)) == name; }

// Auxiliary keyword check: symbol `name` not shadowed in env.
// With Option B alpha-rename, user bindings get gensym names, so this
// is effectively always true unless the user explicitly binds the name
// in a plain (non-expanded) context.
static bool aux_kw(Value v, const char* name, Environment* env)
   {
   if (!sym_is(v, name)) return false;
   return !env || !env->lookup_optional(name).has_value();
   }

// ─────────────────────────────────────────────────────────────────────────────
// Cond clause classifier

enum class CondKind { Else, Arrow, TestOnly, Body };
struct CondClause { CondKind kind; Value test; Value body_or_proc; };

static CondClause classify_cond_clause(Value clause, Environment* env)
   {
   Value head = as_cons(clause)->car;
   if (aux_kw(head, "else", env))
      return {CondKind::Else, head, as_cons(clause)->cdr};
   Value cdr1 = as_cons(clause)->cdr;
   if (is_nil(cdr1)) return {CondKind::TestOnly, head, make_nil()};
   if (is_cons(cdr1))
      {
      Value mid  = as_cons(cdr1)->car;
      Value cdr2 = as_cons(cdr1)->cdr;
      if (aux_kw(mid, "=>", env) && is_cons(cdr2) && is_nil(as_cons(cdr2)->cdr))
         return {CondKind::Arrow, head, as_cons(cdr2)->car};
      }
   return {CondKind::Body, head, cdr1};
   }

// ─────────────────────────────────────────────────────────────────────────────
// Wind walk

static void unwind_winds_on_error(CekCtx& ctx, size_t target_depth)
   {
   while (ctx.wind_stack.size() > target_depth)
      {
      Value after = ctx.wind_stack.back().after;
      ctx.wind_stack.pop_back();
      try { apply_scheme_proc(after, {}, ctx); } catch (...) {}
      }
   }

static void wind_walk(CekCtx& ctx, const std::vector<WindFrame>& target)
   {
   auto& ws = ctx.wind_stack;
   size_t common = 0;
   while (common < ws.size() && common < target.size())
      {
      if (!(ws[common].before == target[common].before && ws[common].after == target[common].after))
         break;
      ++common;
      }
   while (ws.size() > common)
      {
      Value after = ws.back().after; ws.pop_back();
      try { apply_scheme_proc(after, {}, ctx); } catch (...) {}
      }
   for (size_t i = common; i < target.size(); ++i)
      {
      ws.push_back(target[i]);
      try { apply_scheme_proc(target[i].before, {}, ctx); } catch (...) {}
      }
   }

// ─────────────────────────────────────────────────────────────────────────────
// apply_scheme_proc

Value apply_scheme_proc(Value proc, const std::vector<Value>& args,
                         CekCtx& ctx, Environment* /*env*/)
   {
   if (is_builtin(proc))
      {
      ArgBuf buf; for (const auto& a : args) buf.push_back(a);
      return as_builtin(proc)->fn(ArgVec(buf));
      }
   auto pv = apply_parameter_if(proc, (int)args.size());
   if (pv) return *pv;
   if (is_closure(proc) || is_case_closure(proc))
      {
      auto br = beta_reduce_value(proc, args);
      if (!is_cons(br.body)) return make_unspecified();
      if (!is_cons(as_cons(br.body)->cdr))
         return cek_eval(as_cons(br.body)->car, br.new_env, &ctx);
      ConsCell* bs = gc_alloc_cons(); bs->car = make_symbol("begin"); bs->cdr = br.body;
      return cek_eval(make_cons(bs), br.new_env, &ctx);
      }
   throw SchemeTypeError("expected a procedure in apply: " + value_to_string(proc, true));
   }

// ─────────────────────────────────────────────────────────────────────────────
// Error conversion: C++ exception -> Scheme error object Value

static Value exception_to_scheme_value(const SchemeError& e)
   {
   SchemeErrorObject* obj = gc_alloc_error_object();
   obj->message   = e.what();
   obj->irritants = make_nil();
   obj->kind      = 0;
   return make_error_object_val(obj);
   }

// ─────────────────────────────────────────────────────────────────────────────
// The CEK machine main loop

static Value _cek_loop(Value expr, Environment* env, CekCtx& ctx)
   {
   Value C = expr;
   Value V;
   Environment* E = env;
   std::vector<Frame> K;
   bool skip_eval = false;

   // RAII GC hook for K-stack
   struct HookGuard
      {
      std::vector<Frame>& K;
      CekCtx& ctx;
      HookGuard(std::vector<Frame>& k, CekCtx& c) : K(k), ctx(c)
         {
         gc_push_trace_hook([this]()
            {
            for (auto& f : K) trace_frame(f);
            for (auto& v : ctx.handler_stack) gc_trace_value(v);
            for (auto& wf : ctx.wind_stack) { gc_trace_value(wf.before); gc_trace_value(wf.after); }
            });
         gc_push_forward_hook([this]()
            {
            for (auto& f : K) forward_frame(f);
            for (auto& v : ctx.handler_stack) gc_copy_forward_value(v);
            for (auto& wf : ctx.wind_stack) { gc_copy_forward_value(wf.before); gc_copy_forward_value(wf.after); }
            });
         }
      ~HookGuard() { gc_pop_trace_hook(); gc_pop_forward_hook(); }
      } _hooks(K, ctx);

   while (true)
      {
      try
         {
         // ── EVAL ──────────────────────────────────────────────────────────
         if (!skip_eval)
            {
            eval_top:
            if (is_cons(C))
               {
               Value head = as_cons(C)->car;
               if (is_symbol(head))
                  {
                  const std::string& nm = symbol_name(as_symbol_id(head));

                  if (nm == "quote")   { V = as_cons(as_cons(C)->cdr)->car; goto apply_top; }
                  if (nm == "lambda")  { V = make_closure_from_lambda(C, E); goto apply_top; }
                  if (nm == "case-lambda") { V = make_case_closure_from_form(C, E); goto apply_top; }

                  if (nm == "delay" || nm == "delay-force")
                     {
                     Value ex = as_cons(as_cons(C)->cdr)->car;
                     ConsCell* bc = gc_alloc_cons(); bc->car = ex; bc->cdr = make_nil();
                     SchemeClosure* th = gc_alloc_closure(); th->env = E; th->rest_param = NO_REST_PARAM;
                     th->body = make_cons(bc);
                     SchemePromise* p = gc_alloc_promise(make_closure(th));
                     V = make_promise_val(p); goto apply_top;
                     }

                  if (nm == "import")
                     {
                     // Library loading not yet implemented; silently skip.
                     V = make_unspecified(); goto apply_top;
                     }

                  if (nm == "define-library")
                     {
                     throw SchemeSyntaxError("define-library: not yet implemented");
                     }

                  if (nm == "if")
                     {
                     Frame f; f.tag = FTag::If;
                     f.a = as_cons(as_cons(as_cons(C)->cdr)->cdr)->car;   // then
                     // else branch: expander supplies (if test then (quote unspecified)) for 2-arg form
                     Value r3 = as_cons(as_cons(as_cons(C)->cdr)->cdr)->cdr;
                     f.b = is_cons(r3) ? as_cons(r3)->car : make_unspecified();
                     f.env = E;
                     K.push_back(std::move(f));
                     C = as_cons(as_cons(C)->cdr)->car; goto eval_top;
                     }

                  if (nm == "define")
                     {
                     Frame f; f.tag = FTag::Define;
                     f.sid = as_symbol_id(as_cons(as_cons(C)->cdr)->car);
                     f.env = E;
                     K.push_back(std::move(f));
                     C = as_cons(as_cons(as_cons(C)->cdr)->cdr)->car; goto eval_top;
                     }

                  if (nm == "set!")
                     {
                     Frame f; f.tag = FTag::Set;
                     f.sid = as_symbol_id(as_cons(as_cons(C)->cdr)->car);
                     f.env = E;
                     K.push_back(std::move(f));
                     C = as_cons(as_cons(as_cons(C)->cdr)->cdr)->car; goto eval_top;
                     }

                  if (nm == "begin")
                     {
                     Value body = as_cons(C)->cdr;
                     C = as_cons(body)->car;
                     if (is_cons(as_cons(body)->cdr))
                        { Frame f; f.tag=FTag::Seq; f.a=as_cons(body)->cdr; f.env=E; K.push_back(std::move(f)); }
                     goto eval_top;
                     }

                  if (nm == "when")
                     {
                     Frame f; f.tag=FTag::When; f.a=as_cons(as_cons(C)->cdr)->cdr; f.env=E;
                     K.push_back(std::move(f));
                     C = as_cons(as_cons(C)->cdr)->car; goto eval_top;
                     }

                  if (nm == "unless")
                     {
                     Frame f; f.tag=FTag::Unless; f.a=as_cons(as_cons(C)->cdr)->cdr; f.env=E;
                     K.push_back(std::move(f));
                     C = as_cons(as_cons(C)->cdr)->car; goto eval_top;
                     }

                  if (nm == "and")
                     {
                     Value body = as_cons(C)->cdr;
                     if (is_nil(body)) { V = make_bool(true); goto apply_top; }
                     if (is_cons(as_cons(body)->cdr))
                        { Frame f; f.tag=FTag::And; f.a=as_cons(body)->cdr; f.env=E; K.push_back(std::move(f)); }
                     C = as_cons(body)->car; goto eval_top;
                     }

                  if (nm == "or")
                     {
                     Value body = as_cons(C)->cdr;
                     if (is_nil(body)) { V = make_bool(false); goto apply_top; }
                     if (is_cons(as_cons(body)->cdr))
                        { Frame f; f.tag=FTag::Or; f.a=as_cons(body)->cdr; f.env=E; K.push_back(std::move(f)); }
                     C = as_cons(body)->car; goto eval_top;
                     }

                  if (nm == "cond")
                     {
                     Value clauses = as_cons(C)->cdr;
                     Value first   = as_cons(clauses)->car;
                     Value rest    = as_cons(clauses)->cdr;
                     auto  kind    = classify_cond_clause(first, E);
                     if (kind.kind == CondKind::Else)
                        {
                        Value body = kind.body_or_proc;
                        C = as_cons(body)->car;
                        if (is_cons(as_cons(body)->cdr))
                           { Frame f; f.tag=FTag::Seq; f.a=as_cons(body)->cdr; f.env=E; K.push_back(std::move(f)); }
                        goto eval_top;
                        }
                     Frame f; f.tag=FTag::Cond; f.a=first; f.b=rest; f.env=E;
                     K.push_back(std::move(f));
                     C = kind.test; goto eval_top;
                     }

                  if (nm == "case")
                     {
                     Value clauses = as_cons(as_cons(C)->cdr)->cdr;
                     Frame f; f.tag=FTag::Case; f.a=as_cons(clauses)->car; f.b=as_cons(clauses)->cdr; f.env=E;
                     K.push_back(std::move(f));
                     C = as_cons(as_cons(C)->cdr)->car; goto eval_top;
                     }

                  if (nm == "let")
                     {
                     Value r1 = as_cons(C)->cdr;
                     if (is_symbol(as_cons(r1)->car))
                        {
                        // Named let: build closure, bind in loop_env, then apply to inits.
                        uint32_t lsid = as_symbol_id(as_cons(r1)->car);
                        Value r2 = as_cons(r1)->cdr;
                        auto pairs = collect_let_bindings(as_cons(r2)->car);
                        Value body  = as_cons(r2)->cdr;
                        Environment* le = gc_alloc_environment(E);
                        le->bind_id(lsid, make_unspecified());
                        SchemeClosure* cl = gc_alloc_closure();
                        cl->env = le; cl->rest_param = NO_REST_PARAM; cl->body = body;
                        for (const auto& kv : pairs) cl->params.push_back(kv.first);
                        Value clo = make_closure(cl);
                        le->bind_id(lsid, clo);
                        // Now apply clo to the init expressions.
                        std::vector<Value> init_exprs;
                        for (const auto& kv : pairs) init_exprs.push_back(kv.second);
                        V = clo;
                        Frame f; f.tag=FTag::Arg; f.va=std::move(init_exprs); f.env=le; f.b=C;
                        K.push_back(std::move(f));
                        goto apply_top;
                        }
                     // Plain let
                     auto pairs = collect_let_bindings(as_cons(r1)->car);
                     Value body  = as_cons(r1)->cdr;
                     if (pairs.empty())
                        {
                        C = as_cons(body)->car;
                        if (is_cons(as_cons(body)->cdr))
                           { Frame f; f.tag=FTag::Seq; f.a=as_cons(body)->cdr; f.env=E; K.push_back(std::move(f)); }
                        goto eval_top;
                        }
                     std::vector<uint32_t> names; std::vector<Value> vb_rem;
                     for (const auto& kv : pairs) { names.push_back(kv.first); vb_rem.push_back(kv.second); }
                     Value first_expr = vb_rem[0]; vb_rem.erase(vb_rem.begin());
                     Frame f; f.tag=FTag::Let; f.names=std::move(names); f.vb=std::move(vb_rem);
                     f.a=body; f.env=E; K.push_back(std::move(f));
                     C = first_expr; goto eval_top;
                     }

                  if (nm == "let*")
                     {
                     Value r1   = as_cons(C)->cdr;
                     auto pairs = collect_let_bindings(as_cons(r1)->car);
                     Value body = as_cons(r1)->cdr;
                     if (pairs.empty())
                        {
                        C = as_cons(body)->car;
                        if (is_cons(as_cons(body)->cdr))
                           { Frame f; f.tag=FTag::Seq; f.a=as_cons(body)->cdr; f.env=E; K.push_back(std::move(f)); }
                        goto eval_top;
                        }
                     std::vector<std::pair<uint32_t,Value>> rem(pairs.begin()+1, pairs.end());
                     Frame f; f.tag=FTag::LetStar; f.sid=pairs[0].first; f.pairs=std::move(rem);
                     f.a=body; f.env=E; K.push_back(std::move(f));
                     C = pairs[0].second; goto eval_top;
                     }

                  if (nm == "letrec" || nm == "letrec*")
                     {
                     Value r1   = as_cons(C)->cdr;
                     auto pairs = collect_let_bindings(as_cons(r1)->car);
                     Value body = as_cons(r1)->cdr;
                     if (pairs.empty())
                        {
                        C = as_cons(body)->car;
                        if (is_cons(as_cons(body)->cdr))
                           { Frame f; f.tag=FTag::Seq; f.a=as_cons(body)->cdr; f.env=E; K.push_back(std::move(f)); }
                        goto eval_top;
                        }
                     Environment* ne = gc_alloc_environment(E);
                     for (const auto& kv : pairs) ne->bind_id(kv.first, make_unspecified());
                     std::vector<std::pair<uint32_t,Value>> rem(pairs.begin()+1, pairs.end());
                     Frame f; f.tag=FTag::Letrec; f.sid=pairs[0].first; f.pairs=std::move(rem);
                     f.a=body; f.env=ne; K.push_back(std::move(f));
                     C = pairs[0].second; E = ne; goto eval_top;
                     }

                  // Application with symbol head
                  {
                  auto args_v = collect_cons(as_cons(C)->cdr);
                  Frame f; f.tag=FTag::Arg; f.va=std::move(args_v); f.env=E; f.b=C;
                  K.push_back(std::move(f));
                  C = head; goto eval_top;
                  }
                  } // is_symbol(head)

               // head is not a symbol — application (e.g., immediate lambda)
               {
               auto args_v = collect_cons(as_cons(C)->cdr);
               Frame f; f.tag=FTag::Arg; f.va=std::move(args_v); f.env=E; f.b=C;
               K.push_back(std::move(f));
               C = head; goto eval_top;
               }
               } // is_cons(C)

            if (is_symbol(C))
               {
               V = E->lookup_id(as_symbol_id(C));
               goto apply_top;
               }

            // Self-evaluating atom
            V = C;
            goto apply_top;
            } // !skip_eval
         else
            {
            skip_eval = false;
            goto apply_top;
            }

         // ── APPLY ─────────────────────────────────────────────────────────
         apply_top:
         while (true)
            {
            if (K.empty()) return V;

            if (is_multi_values(V) && !multi_values_ok_tag(K.back().tag))
               throw SchemeTypeError("multiple values delivered to a single-value context");

            Frame fr = std::move(K.back()); K.pop_back();

            switch (fr.tag)
               {

               case FTag::Define:
                  fr.env->bind_id(fr.sid, V);
                  V = make_unspecified();
                  continue;

               case FTag::Set:
                  fr.env->set_id(fr.sid, V);
                  V = make_unspecified();
                  continue;

               case FTag::If:
                  C = is_truthy(V) ? fr.a : fr.b;
                  E = fr.env;
                  goto eval_top;

               case FTag::Seq:
                  E = fr.env;
                  C = as_cons(fr.a)->car;
                  if (is_cons(as_cons(fr.a)->cdr))
                     { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(fr.a)->cdr; nf.env=E; K.push_back(std::move(nf)); }
                  goto eval_top;

               case FTag::When:
                  if (!is_truthy(V)) { V = make_unspecified(); continue; }
                  E = fr.env;
                  C = as_cons(fr.a)->car;
                  if (is_cons(as_cons(fr.a)->cdr))
                     { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(fr.a)->cdr; nf.env=E; K.push_back(std::move(nf)); }
                  goto eval_top;

               case FTag::Unless:
                  if (is_truthy(V)) { V = make_unspecified(); continue; }
                  E = fr.env;
                  C = as_cons(fr.a)->car;
                  if (is_cons(as_cons(fr.a)->cdr))
                     { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(fr.a)->cdr; nf.env=E; K.push_back(std::move(nf)); }
                  goto eval_top;

               case FTag::And:
                  if (!is_truthy(V)) continue;   // short-circuit, V stays #f
                  if (is_nil(fr.a))  continue;   // V is last truthy value
                  if (is_cons(as_cons(fr.a)->cdr))
                     { Frame nf; nf.tag=FTag::And; nf.a=as_cons(fr.a)->cdr; nf.env=fr.env; K.push_back(std::move(nf)); }
                  C = as_cons(fr.a)->car; E = fr.env; goto eval_top;

               case FTag::Or:
                  if (is_truthy(V)) continue;    // short-circuit, V stays truthy
                  if (is_nil(fr.a)) continue;    // V stays #f
                  if (is_cons(as_cons(fr.a)->cdr))
                     { Frame nf; nf.tag=FTag::Or; nf.a=as_cons(fr.a)->cdr; nf.env=fr.env; K.push_back(std::move(nf)); }
                  C = as_cons(fr.a)->car; E = fr.env; goto eval_top;

               case FTag::Cond:
                  {
                  Value cur_clause  = fr.a;
                  Value rem_clauses = fr.b;
                  E = fr.env;
                  if (!is_truthy(V))
                     {
                     // Test failed - advance
                     if (is_nil(rem_clauses)) { V = make_unspecified(); continue; }
                     Value nxt  = as_cons(rem_clauses)->car;
                     Value rest = as_cons(rem_clauses)->cdr;
                     auto kind  = classify_cond_clause(nxt, fr.env);
                     if (kind.kind == CondKind::Else)
                        {
                        Value body = kind.body_or_proc;
                        C = as_cons(body)->car;
                        if (is_cons(as_cons(body)->cdr))
                           { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(body)->cdr; nf.env=E; K.push_back(std::move(nf)); }
                        goto eval_top;
                        }
                     Frame nf; nf.tag=FTag::Cond; nf.a=nxt; nf.b=rest; nf.env=fr.env; K.push_back(std::move(nf));
                     C = kind.test; goto eval_top;
                     }
                  // Test truthy
                  auto kind = classify_cond_clause(cur_clause, fr.env);
                  if (kind.kind == CondKind::TestOnly) continue;
                  if (kind.kind == CondKind::Body)
                     {
                     Value body = kind.body_or_proc;
                     C = as_cons(body)->car;
                     if (is_cons(as_cons(body)->cdr))
                        { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(body)->cdr; nf.env=E; K.push_back(std::move(nf)); }
                     goto eval_top;
                     }
                  // Arrow
                  { Frame nf; nf.tag=FTag::CondArrow; nf.a=V; nf.env=fr.env; K.push_back(std::move(nf)); }
                  C = kind.body_or_proc; goto eval_top;
                  }

               case FTag::CondArrow:
                  {
                  Value test_val = fr.a;
                  if (is_continuation(V))
                     {
                     wind_walk(ctx, as_continuation(V)->wind_stack);
                     K = *static_cast<std::vector<Frame>*>(as_continuation(V)->frames_ptr);
                     V = continuation_value({test_val});
                     continue;
                     }
                  auto pv = apply_parameter_if(V, 1);
                  if (pv) { V = *pv; continue; }
                  if (is_builtin(V)) { ArgBuf b; b.push_back(test_val); V = as_builtin(V)->fn(ArgVec(b)); continue; }
                  { auto r = beta_reduce_value(V, {test_val});
                    C = as_cons(r.body)->car; E = r.new_env;
                    if (is_cons(as_cons(r.body)->cdr)) { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(r.body)->cdr; nf.env=E; K.push_back(std::move(nf)); }
                    goto eval_top; }
                  }

               case FTag::Case:
                  {
                  Value key  = V;
                  Value head = as_cons(fr.a)->car;
                  E = fr.env;
                  if (aux_kw(head, "else", fr.env))
                     {
                     Value body = as_cons(fr.a)->cdr;
                     if (is_cons(body) && sym_is(as_cons(body)->car, "=>"))
                        {
                        Frame nf; nf.tag=FTag::CaseArrow; nf.a=key; nf.env=fr.env; K.push_back(std::move(nf));
                        C = as_cons(as_cons(body)->cdr)->car; goto eval_top;
                        }
                     C = as_cons(body)->car;
                     if (is_cons(as_cons(body)->cdr))
                        { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(body)->cdr; nf.env=E; K.push_back(std::move(nf)); }
                     goto eval_top;
                     }
                  // Datum-list match
                  bool matched = false;
                  Value dcur = head;
                  while (is_cons(dcur)) { if (values_eqv(key, as_cons(dcur)->car)) { matched = true; break; } dcur = as_cons(dcur)->cdr; }
                  if (matched)
                     {
                     Value body = as_cons(fr.a)->cdr;
                     if (is_cons(body) && sym_is(as_cons(body)->car, "=>"))
                        {
                        Frame nf; nf.tag=FTag::CaseArrow; nf.a=key; nf.env=fr.env; K.push_back(std::move(nf));
                        C = as_cons(as_cons(body)->cdr)->car; goto eval_top;
                        }
                     C = as_cons(body)->car;
                     if (is_cons(as_cons(body)->cdr))
                        { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(body)->cdr; nf.env=E; K.push_back(std::move(nf)); }
                     goto eval_top;
                     }
                  if (is_nil(fr.b)) { V = make_unspecified(); continue; }
                  { Frame nf; nf.tag=FTag::Case; nf.a=as_cons(fr.b)->car; nf.b=as_cons(fr.b)->cdr; nf.env=fr.env; K.push_back(std::move(nf)); }
                  continue;  // V stays as key
                  }

               case FTag::CaseArrow:
                  {
                  Value key_val = fr.a;
                  if (is_continuation(V))
                     {
                     wind_walk(ctx, as_continuation(V)->wind_stack);
                     K = *static_cast<std::vector<Frame>*>(as_continuation(V)->frames_ptr);
                     V = continuation_value({key_val});
                     continue;
                     }
                  auto pv = apply_parameter_if(V, 1);
                  if (pv) { V = *pv; continue; }
                  if (is_builtin(V)) { ArgBuf b; b.push_back(key_val); V = as_builtin(V)->fn(ArgVec(b)); continue; }
                  { auto r = beta_reduce_value(V, {key_val});
                    C = as_cons(r.body)->car; E = r.new_env;
                    if (is_cons(as_cons(r.body)->cdr)) { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(r.body)->cdr; nf.env=E; K.push_back(std::move(nf)); }
                    goto eval_top; }
                  }

               case FTag::Let:
                  {
                  fr.va.push_back(V);
                  if (fr.vb.empty())
                     {
                     // All values collected; build env and run body
                     Environment* ne = gc_alloc_environment(fr.env);
                     for (size_t i = 0; i < fr.names.size(); ++i) ne->bind_id(fr.names[i], fr.va[i]);
                     E = ne; C = as_cons(fr.a)->car;
                     if (is_cons(as_cons(fr.a)->cdr))
                        { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(fr.a)->cdr; nf.env=ne; K.push_back(std::move(nf)); }
                     goto eval_top;
                     }
                  Value next_expr = fr.vb[0]; fr.vb.erase(fr.vb.begin());
                  K.push_back(std::move(fr));
                  C = next_expr; E = K.back().env; goto eval_top;
                  }

               case FTag::LetStar:
                  {
                  Environment* ne = gc_alloc_environment(fr.env);
                  ne->bind_id(fr.sid, V);
                  if (fr.pairs.empty())
                     {
                     E = ne; C = as_cons(fr.a)->car;
                     if (is_cons(as_cons(fr.a)->cdr))
                        { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(fr.a)->cdr; nf.env=ne; K.push_back(std::move(nf)); }
                     goto eval_top;
                     }
                  Value next_expr = fr.pairs[0].second;
                  Frame nf; nf.tag=FTag::LetStar; nf.sid=fr.pairs[0].first;
                  nf.pairs = std::vector<std::pair<uint32_t,Value>>(fr.pairs.begin()+1, fr.pairs.end());
                  nf.a=fr.a; nf.env=ne; K.push_back(std::move(nf));
                  C = next_expr; E = ne; goto eval_top;
                  }

               case FTag::Letrec:
                  {
                  fr.env->set_id(fr.sid, V);
                  if (fr.pairs.empty())
                     {
                     E = fr.env; C = as_cons(fr.a)->car;
                     if (is_cons(as_cons(fr.a)->cdr))
                        { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(fr.a)->cdr; nf.env=fr.env; K.push_back(std::move(nf)); }
                     goto eval_top;
                     }
                  Value next_expr = fr.pairs[0].second;
                  Frame nf; nf.tag=FTag::Letrec; nf.sid=fr.pairs[0].first;
                  nf.pairs = std::vector<std::pair<uint32_t,Value>>(fr.pairs.begin()+1, fr.pairs.end());
                  nf.a=fr.a; nf.env=fr.env; K.push_back(std::move(nf));
                  C = next_expr; E = fr.env; goto eval_top;
                  }

               case FTag::DynWindAfter:
                  {
                  Value body_result = V;
                  if (!ctx.wind_stack.empty()) ctx.wind_stack.pop_back();
                  apply_scheme_proc(fr.a, {}, ctx);
                  V = body_result; continue;
                  }

               case FTag::CwvConsumer:
                  {
                  Value consumer = fr.a;
                  std::vector<Value> cargs;
                  if (is_multi_values(V))
                     cargs = as_multi_values(V)->values;
                  else
                     cargs.push_back(V);
                  if (is_continuation(consumer))
                     {
                     wind_walk(ctx, as_continuation(consumer)->wind_stack);
                     K = *static_cast<std::vector<Frame>*>(as_continuation(consumer)->frames_ptr);
                     V = continuation_value(cargs); continue;
                     }
                  auto pv = apply_parameter_if(consumer, (int)cargs.size());
                  if (pv) { V = *pv; continue; }
                  if (is_builtin(consumer)) { ArgBuf b; for (auto& a:cargs) b.push_back(a); V = as_builtin(consumer)->fn(ArgVec(b)); continue; }
                  { auto r = beta_reduce_value(consumer, cargs);
                    C = as_cons(r.body)->car; E = r.new_env;
                    if (is_cons(as_cons(r.body)->cdr)) { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(r.body)->cdr; nf.env=E; K.push_back(std::move(nf)); }
                    goto eval_top; }
                  }

               case FTag::ForceResult:
                  {
                  SchemePromise* p = as_promise(fr.a);
                  if (is_promise(V))
                     {
                     // promise_become: make p share state with V's promise
                     SchemePromise* vp = as_promise(V);
                     p->forced = vp->forced;
                     p->val    = vp->val;
                     if (p->forced) { V = p->val; continue; }
                     // Iterate: push another ForceResult and call the thunk
                     { Frame nf; nf.tag=FTag::ForceResult; nf.a=fr.a; K.push_back(std::move(nf)); }
                     Value thunk = p->val;
                     auto pv2 = apply_parameter_if(thunk, 0);
                     if (pv2) { V = *pv2; continue; }
                     if (is_builtin(thunk)) { V = as_builtin(thunk)->fn(ArgVec{}); continue; }
                     { auto r = beta_reduce_value(thunk, {});
                       C = as_cons(r.body)->car; E = r.new_env;
                       if (is_cons(as_cons(r.body)->cdr)) { Frame nf2; nf2.tag=FTag::Seq; nf2.a=as_cons(r.body)->cdr; nf2.env=E; K.push_back(std::move(nf2)); }
                       goto eval_top; }
                     }
                  // Resolve promise
                  p->forced = true; p->val = V; continue;
                  }

               case FTag::MakeParam:
                  V = make_parameter_val(gc_alloc_parameter(V, fr.a));
                  continue;

               case FTag::PopHandler:
                  if (!ctx.handler_stack.empty()) ctx.handler_stack.pop_back();
                  continue;

               case FTag::ReinstallHandler:
                  ctx.handler_stack.push_back(fr.a);
                  continue;

               case FTag::ShadowPop:
                  if (!ctx.shadow_stack.empty()) ctx.shadow_stack.pop_back();
                  continue;

               case FTag::TraceExit:
                  continue;   // tracing not yet active

               case FTag::NonContinReturn:
                  {
                  SchemeErrorObject* obj = gc_alloc_error_object();
                  obj->message = "exception handler returned from non-continuable raise";
                  obj->irritants = make_nil();  // TODO: could include fr.a
                  obj->kind = 0;
                  throw SchemeRaisedException(make_error_object_val(obj), false);
                  }

               case FTag::Arg:
                  {
                  // V = the function, fr.va = arg expressions, fr.env = call-site env, fr.b = app_node
                  std::vector<Value>& args_list = fr.va;
                  Environment* saved_env = fr.env;
                  Value app_node = fr.b;
                  if (args_list.empty())
                     {
                     // 0-arg call
                     if (is_continuation(V))
                        {
                        wind_walk(ctx, as_continuation(V)->wind_stack);
                        ctx.handler_stack = as_continuation(V)->arg_stack_snapshot;  // reuse field for handler snapshot
                        K = *static_cast<std::vector<Frame>*>(as_continuation(V)->frames_ptr);
                        V = continuation_value({}); continue;
                        }
                     auto pv = apply_parameter_if(V, 0);
                     if (pv) { V = *pv; continue; }
                     if (is_builtin(V)) { V = as_builtin(V)->fn(ArgVec{}); continue; }
                     { auto r = beta_reduce_value(V, {});
                       C = as_cons(r.body)->car; E = r.new_env;
                       if (is_cons(as_cons(r.body)->cdr)) { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(r.body)->cdr; nf.env=E; K.push_back(std::move(nf)); }
                       goto eval_top; }
                     }
                  // Has args: push CALL baton and evaluate first arg
                  Value first = args_list[0];
                  std::vector<Value> remaining(args_list.begin()+1, args_list.end());
                  Frame cf; cf.tag=FTag::Call; cf.a=V; cf.vb=std::move(remaining); cf.env=saved_env; cf.b=app_node;
                  K.push_back(std::move(cf));
                  C = first; E = saved_env; goto eval_top;
                  }

               case FTag::Call:
                  {
                  Value fn_val   = fr.a;
                  fr.va.push_back(V);
                  std::vector<Value>& collected = fr.va;
                  std::vector<Value>& remaining = fr.vb;
                  Environment* saved_env = fr.env;
                  Value app_node = fr.b;

                  if (!remaining.empty())
                     {
                     Value next = remaining[0]; remaining.erase(remaining.begin());
                     K.push_back(std::move(fr));
                     C = next; E = saved_env; goto eval_top;
                     }

                  // All args collected — dispatch
                  if (is_continuation(fn_val))
                     {
                     wind_walk(ctx, as_continuation(fn_val)->wind_stack);
                     K = *static_cast<std::vector<Frame>*>(as_continuation(fn_val)->frames_ptr);
                     V = continuation_value(collected); continue;
                     }

                  // call/cc
                  if (is_builtin_named(fn_val, "call-with-current-continuation")
                      || is_builtin_named(fn_val, "call/cc"))
                     {
                     if (collected.size() != 1)
                        throw SchemeArityError(arity_mismatch_msg(as_builtin(fn_val)->name, 1, 1, (int)collected.size()));
                     SchemeContinuation* cont = gc_alloc_continuation();
                     cont->frames_ptr = new std::vector<Frame>(K);
                     cont->wind_stack = ctx.wind_stack;
                     cont->arg_stack_snapshot = ctx.handler_stack;  // reuse for handler snapshot
                     fn_val = collected[0];
                     collected = {make_continuation(cont)};
                     }

                  // apply (loop in case of (apply apply ...))
                  while (is_builtin_named(fn_val, "apply"))
                     {
                     if (collected.size() < 2)
                        throw SchemeArityError(arity_mismatch_msg("apply", 2, -1, (int)collected.size()));
                     Value proc = collected[0];
                     std::vector<Value> flat_args(collected.begin()+1, collected.end()-1);
                     Value last = collected.back();
                     Value cur = last;
                     while (is_cons(cur)) { flat_args.push_back(as_cons(cur)->car); cur = as_cons(cur)->cdr; }
                     if (!is_nil(cur)) throw SchemeTypeError("apply: last argument must be a proper list");
                     fn_val    = proc;
                     collected = std::move(flat_args);
                     }

                  // call-with-values
                  if (is_builtin_named(fn_val, "call-with-values"))
                     {
                     if (collected.size() != 2) throw SchemeArityError(arity_mismatch_msg("call-with-values",2,2,(int)collected.size()));
                     Value producer = collected[0], consumer = collected[1];
                     Frame nf; nf.tag=FTag::CwvConsumer; nf.a=consumer; nf.b=app_node; K.push_back(std::move(nf));
                     fn_val = producer; collected.clear();
                     }

                  // force
                  if (is_builtin_named(fn_val, "force"))
                     {
                     if (collected.size() != 1) throw SchemeArityError(arity_mismatch_msg("force",1,1,(int)collected.size()));
                     Value p = collected[0];
                     if (!is_promise(p)) { V = p; continue; }
                     if (as_promise(p)->forced) { V = as_promise(p)->val; continue; }
                     Frame nf; nf.tag=FTag::ForceResult; nf.a=p; K.push_back(std::move(nf));
                     fn_val = as_promise(p)->val; collected.clear();
                     }

                  // make-parameter
                  if (is_builtin_named(fn_val, "make-parameter"))
                     {
                     if (collected.size() < 1 || collected.size() > 2)
                        throw SchemeArityError(arity_mismatch_msg("make-parameter",1,2,(int)collected.size()));
                     if (collected.size() == 1) { V = make_parameter_val(gc_alloc_parameter(collected[0], make_unspecified())); continue; }
                     Value converter = collected[1];
                     Frame nf; nf.tag=FTag::MakeParam; nf.a=converter; K.push_back(std::move(nf));
                     fn_val = converter; collected = {collected[0]};
                     }

                  // with-exception-handler
                  if (is_builtin_named(fn_val, "with-exception-handler"))
                     {
                     if (collected.size() != 2) throw SchemeArityError(arity_mismatch_msg("with-exception-handler",2,2,(int)collected.size()));
                     ctx.handler_stack.push_back(collected[0]);
                     Frame nf; nf.tag=FTag::PopHandler; K.push_back(std::move(nf));
                     fn_val = collected[1]; collected.clear();
                     }

                  // raise
                  if (is_builtin_named(fn_val, "raise"))
                     {
                     if (collected.size() != 1) throw SchemeArityError(arity_mismatch_msg("raise",1,1,(int)collected.size()));
                     throw SchemeRaisedException(collected[0], false);
                     }

                  // raise-continuable
                  if (is_builtin_named(fn_val, "raise-continuable"))
                     {
                     if (collected.size() != 1) throw SchemeArityError(arity_mismatch_msg("raise-continuable",1,1,(int)collected.size()));
                     Value rv = collected[0];
                     if (ctx.handler_stack.empty()) throw SchemeRaisedException(rv, true);
                     Value handler = ctx.handler_stack.back(); ctx.handler_stack.pop_back();
                     Frame nf; nf.tag=FTag::ReinstallHandler; nf.a=handler; K.push_back(std::move(nf));
                     fn_val = handler; collected = {rv};
                     }

                  // eval
                  if (is_builtin_named(fn_val, "eval"))
                     {
                     if (collected.size() < 1 || collected.size() > 2)
                        throw SchemeArityError(arity_mismatch_msg("eval",1,2,(int)collected.size()));
                     Value datum = collected[0];
                     Environment* target_env = saved_env ? saved_env->global : nullptr;
                     if (collected.size() == 2)
                        {
                        if (!is_environment(collected[1])) throw SchemeTypeError("eval: second argument must be an environment");
                        target_env = as_environment_val(collected[1]);
                        }
                     if (!target_env) throw SchemeTypeError("eval: no environment");
                     Value expanded = expand(datum, target_env);
                     StaticEnv se; seed_static_env(se);
                     analyze(expanded, &se);
                     C = expanded; E = target_env; goto eval_top;
                     }

                  // error
                  if (is_builtin_named(fn_val, "error"))
                     {
                     if (collected.empty()) throw SchemeArityError(arity_mismatch_msg("error",1,-1,0));
                     if (!is_string(collected[0])) throw SchemeTypeError("error: first argument must be a string");
                     std::string msg = as_string(collected[0])->data;
                     SchemeErrorObject* obj = gc_alloc_error_object();
                     obj->message = msg;
                     ConsCell* irr = nullptr;
                     for (int i = (int)collected.size()-1; i >= 1; --i)
                        { ConsCell* c = gc_alloc_cons(); c->car=collected[i]; c->cdr=(irr?make_cons(irr):make_nil()); irr=c; }
                     obj->irritants = irr ? make_cons(irr) : make_nil();
                     obj->kind = 0;
                     throw SchemeRaisedException(make_error_object_val(obj), false);
                     }

                  // dynamic-wind
                  if (is_builtin_named(fn_val, "dynamic-wind"))
                     {
                     if (collected.size() != 3) throw SchemeArityError(arity_mismatch_msg("dynamic-wind",3,3,(int)collected.size()));
                     Value before = collected[0], thunk = collected[1], after = collected[2];
                     apply_scheme_proc(before, {}, ctx);
                     ctx.wind_stack.push_back({before, after});
                     Frame nf; nf.tag=FTag::DynWindAfter; nf.a=after; K.push_back(std::move(nf));
                     fn_val = thunk; collected.clear();
                     }

                  // %with-parameters (parameterize desugars to this)
                  if (is_builtin_named(fn_val, "%with-parameters"))
                     {
                     if (collected.size() != 3) throw SchemeArityError(arity_mismatch_msg("%with-parameters",3,3,(int)collected.size()));
                     Value params_list = collected[0], vals_list = collected[1], body_thunk = collected[2];
                     std::vector<SchemeParameter*> pv_ptrs;
                     std::vector<Value> new_vals, saved_vals;
                     Value pc = params_list;
                     while (is_cons(pc)) { if (!is_parameter(as_cons(pc)->car)) throw SchemeTypeError("%with-parameters: non-parameter"); pv_ptrs.push_back(as_parameter(as_cons(pc)->car)); pc = as_cons(pc)->cdr; }
                     Value vc = vals_list;
                     while (is_cons(vc)) { new_vals.push_back(as_cons(vc)->car); vc = as_cons(vc)->cdr; }
                     if (pv_ptrs.size() != new_vals.size()) throw SchemeTypeError("%with-parameters: count mismatch");
                     for (auto* pp : pv_ptrs) saved_vals.push_back(pp->current);
                     for (size_t i = 0; i < pv_ptrs.size(); ++i) pv_ptrs[i]->current = new_vals[i];
                     // Build restore thunk as a builtin lambda
                     struct RestoreData {
                        std::vector<SchemeParameter*> ptrs;
                        std::vector<Value>            saved;
                     };
                     auto* rd = new RestoreData{pv_ptrs, saved_vals};
                     Builtin* restore_b = new Builtin("%parameterize-restore", [rd](ArgVec) -> Value {
                        for (size_t i = 0; i < rd->ptrs.size(); ++i) rd->ptrs[i]->current = rd->saved[i];
                        delete rd;
                        return make_unspecified();
                        });
                     Value restore_prim = make_builtin(restore_b);
                     WindFrame wf; wf.before = make_unspecified(); wf.after = restore_prim;
                     ctx.wind_stack.push_back(wf);
                     Frame nf; nf.tag=FTag::DynWindAfter; nf.a=restore_prim; K.push_back(std::move(nf));
                     fn_val = body_thunk; collected.clear();
                     }

                  // parameter object
                  auto ppv = apply_parameter_if(fn_val, (int)collected.size());
                  if (ppv) { V = *ppv; continue; }

                  // builtin
                  if (is_builtin(fn_val))
                     {
                     ArgBuf buf; for (auto& a : collected) buf.push_back(a);
                     V = as_builtin(fn_val)->fn(ArgVec(buf)); continue;
                     }

                  // closure / case-closure
                  { auto r = beta_reduce_value(fn_val, collected);
                    C = as_cons(r.body)->car; E = r.new_env;
                    if (is_cons(as_cons(r.body)->cdr)) { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(r.body)->cdr; nf.env=E; K.push_back(std::move(nf)); }
                    goto eval_top; }
                  }

               } // switch
            } // apply while
         } // try
      catch (const SchemeRaisedException& e)
         {
         // Walk K for FRAME_POP_HANDLER, running DynWindAfter along the way.
         Value handler_val;
         bool found = false;
         while (!K.empty())
            {
            Frame& top = K.back();
            if (top.tag == FTag::PopHandler)
               {
               K.pop_back();
               if (!ctx.handler_stack.empty()) { handler_val = ctx.handler_stack.back(); ctx.handler_stack.pop_back(); found = true; }
               break;
               }
            if (top.tag == FTag::ReinstallHandler) { K.pop_back(); continue; }
            if (top.tag == FTag::DynWindAfter)
               {
               Value after = top.a; K.pop_back();
               if (!ctx.wind_stack.empty()) ctx.wind_stack.pop_back();
               try { apply_scheme_proc(after, {}, ctx); } catch (...) {}
               continue;
               }
            K.pop_back();
            }
         if (!found) throw;
         Value raised_val = e.raised;
         if (!e.continuable)
            { Frame nf; nf.tag=FTag::NonContinReturn; nf.a=raised_val; K.push_back(std::move(nf)); }
         // Dispatch handler: may be builtin, closure, or continuation.
         auto ppv = apply_parameter_if(handler_val, 1);
         if (ppv) { V = *ppv; skip_eval = true; continue; }
         if (is_builtin(handler_val))
            { ArgBuf b; b.push_back(raised_val); V = as_builtin(handler_val)->fn(ArgVec(b)); skip_eval = true; continue; }
         { auto r = beta_reduce_value(handler_val, {raised_val});
           C = as_cons(r.body)->car; E = r.new_env;
           if (is_cons(as_cons(r.body)->cdr)) { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(r.body)->cdr; nf.env=E; K.push_back(std::move(nf)); }
           continue; }
         }
      catch (const SchemeError& e)
         {
         // Convert to SchemeRaisedException and re-enter the handler dispatch.
         // If no handler, re-throw the original.
         Value errval = exception_to_scheme_value(e);
         // Walk K for handler frame
         Value handler_val;
         bool found = false;
         while (!K.empty())
            {
            Frame& top = K.back();
            if (top.tag == FTag::PopHandler)
               {
               K.pop_back();
               if (!ctx.handler_stack.empty()) { handler_val = ctx.handler_stack.back(); ctx.handler_stack.pop_back(); found = true; }
               break;
               }
            if (top.tag == FTag::ReinstallHandler) { K.pop_back(); continue; }
            if (top.tag == FTag::DynWindAfter)
               {
               Value after = top.a; K.pop_back();
               if (!ctx.wind_stack.empty()) ctx.wind_stack.pop_back();
               try { apply_scheme_proc(after, {}, ctx); } catch (...) {}
               continue;
               }
            K.pop_back();
            }
         if (!found) throw;
         // Dispatch handler
         auto ppv = apply_parameter_if(handler_val, 1);
         if (ppv) { V = *ppv; skip_eval = true; continue; }
         if (is_builtin(handler_val))
            { ArgBuf b; b.push_back(errval); V = as_builtin(handler_val)->fn(ArgVec(b)); skip_eval = true; continue; }
         { auto r = beta_reduce_value(handler_val, {errval});
           C = as_cons(r.body)->car; E = r.new_env;
           if (is_cons(as_cons(r.body)->cdr)) { Frame nf; nf.tag=FTag::Seq; nf.a=as_cons(r.body)->cdr; nf.env=E; K.push_back(std::move(nf)); }
           continue; }
         }
      } // outer while(true)
   }

// ─────────────────────────────────────────────────────────────────────────────
// Public entry point

Value cek_eval(Value expr, Environment* env, CekCtx* ctx_ptr)
   {
   static thread_local CekCtx default_ctx;
   CekCtx& ctx = ctx_ptr ? *ctx_ptr : default_ctx;

   size_t wind_depth    = ctx.wind_stack.size();
   size_t handler_depth = ctx.handler_stack.size();

   try
      {
      return _cek_loop(expr, env, ctx);
      }
   catch (...)
      {
      unwind_winds_on_error(ctx, wind_depth);
      while (ctx.handler_stack.size() > handler_depth) ctx.handler_stack.pop_back();
      ctx.shadow_stack.clear();
      throw;
      }
   }
