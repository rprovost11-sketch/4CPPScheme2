// gc_gen.cpp — Generational GC: bump-pointer nursery + concurrent tri-color major GC
//              with incremental sweep.
// Adapted from cppScheme. Changes vs cppScheme:
//   - No profiler dependency.
//   - Environment uses bindings_ (unordered_map<uint32_t,Value>) not the small-array layout.
//   - Added GC handling for CaseClosure, MultiValues, Record, ExactComplex, ErrorObject.

#include "gc.h"
#include "environment.h"
#include "port.h"
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cassert>
#include <thread>
#include <mutex>
#include <condition_variable>

// ── Nursery ───────────────────────────────────────────────────────────────────

static constexpr size_t NURSERY_CAPACITY = 8192;
static ConsCell  g_nursery[NURSERY_CAPACITY];
static size_t    g_nursery_bump = 0;

// ── State ─────────────────────────────────────────────────────────────────────

static GcHeader* g_young_head  = nullptr;
static size_t    g_young_count = 0;
static size_t    g_young_threshold = 256;

static GcHeader* g_old_head    = nullptr;
static size_t    g_old_count   = 0;
static size_t    g_old_threshold = 1024;

static std::unordered_set<GcHeader*> g_remembered_set;
static bool g_minor_gc_active = false;

static std::vector<Value*>        g_value_roots;
static std::vector<Environment**> g_env_roots;
static std::vector<GcTraceHook>   g_trace_hooks;
static std::vector<GcForwardHook> g_forward_hooks;

// ── Concurrent major GC state ─────────────────────────────────────────────────

enum class GcPhase { Idle, Marking, Sweeping };

static std::atomic<GcPhase>    g_gc_phase{GcPhase::Idle};
static std::vector<GcHeader*>  g_gray_stack;
static std::mutex              g_mark_mutex;
static std::condition_variable g_mark_cv;
static std::atomic<size_t>     g_gray_inflight{0};
static std::thread             g_gc_thread;
static std::atomic<bool>       g_gc_thread_stop{false};

// ── Incremental sweep state ───────────────────────────────────────────────────

static GcHeader**        g_sweep_cursor    = nullptr;
static constexpr size_t  SWEEP_STEP_BUDGET = 512;

// ── Forward declarations ──────────────────────────────────────────────────────
static void shade_gray(GcHeader*);
static void process_gray_object(GcHeader*);
static void gc_major_finish();
static void minor_collect();

// ── Write barrier ─────────────────────────────────────────────────────────────

void gc_write_barrier(GcHeader* parent, GcHeader* child)
   {
   if (!parent || !child) return;
   if (parent->gen == 1 && child->gen == 0)
      g_remembered_set.insert(parent);
   if (g_gc_phase.load(std::memory_order_relaxed) == GcPhase::Marking
         && parent->marked.load(std::memory_order_relaxed)
         && child->gen == 1)
      shade_gray(child);
   }

// ── Pointer forwarding ────────────────────────────────────────────────────────

void gc_copy_forward_value(Value& val)  { gc_forward_value(val); }
void gc_copy_forward_env(Environment*&) {}

// ── Shade helpers ─────────────────────────────────────────────────────────────

static void shade_gray(GcHeader* header)
   {
   if (!header || header->gen != 1) return;
   bool expected = false;
   if (!header->marked.compare_exchange_strong(
         expected, true, std::memory_order_acq_rel, std::memory_order_relaxed))
      return;
   g_gray_inflight.fetch_add(1, std::memory_order_relaxed);
   {
   std::lock_guard<std::mutex> lk(g_mark_mutex);
   g_gray_stack.push_back(header);
   }
   g_mark_cv.notify_one();
   }

static void shade_gray_value(Value val) { shade_gray(gc_value_header(val)); }

// ── Background marking thread ─────────────────────────────────────────────────

static void gc_marking_thread_fn()
   {
   while (true)
      {
      GcHeader* obj = nullptr;
      {
      std::unique_lock<std::mutex> lk(g_mark_mutex);
      g_mark_cv.wait(lk, []
         { return !g_gray_stack.empty() || g_gc_thread_stop.load(std::memory_order_relaxed); });
      if (g_gc_thread_stop.load(std::memory_order_relaxed)) return;
      obj = g_gray_stack.back();
      g_gray_stack.pop_back();
      }
      process_gray_object(obj);
      g_gray_inflight.fetch_sub(1, std::memory_order_acq_rel);
      }
   }

// ── process_gray_object ───────────────────────────────────────────────────────

static void process_gray_object(GcHeader* header)
   {
   switch (header->type)
      {
      case GcType::Cons:
         {
         auto* cell = reinterpret_cast<ConsCell*>(header);
         shade_gray_value(cell->car);
         shade_gray_value(cell->cdr);
         break;
         }
      case GcType::String:    break;
      case GcType::Closure:
         {
         auto* cl = reinterpret_cast<SchemeClosure*>(header);
         shade_gray_value(cl->body);
         if (cl->env) shade_gray(&cl->env->header);
         break;
         }
      case GcType::CaseClosure:
         {
         auto* cc = reinterpret_cast<SchemeCaseClosure*>(header);
         for (auto& clause : cc->clauses) shade_gray_value(clause.body);
         if (cc->env) shade_gray(&cc->env->header);
         break;
         }
      case GcType::Macro:
         {
         auto* m = reinterpret_cast<SchemeMacro*>(header);
         shade_gray_value(m->body);
         if (m->env) shade_gray(&m->env->header);
         break;
         }
      case GcType::SyntaxTransformer:
         {
         auto* t = reinterpret_cast<SchemeSyntaxTransformer*>(header);
         for (auto& rule : t->rules)
            { shade_gray_value(rule.pattern); shade_gray_value(rule.tmpl); }
         if (t->def_env) shade_gray(&t->def_env->header);
         break;
         }
      case GcType::Continuation:
         {
         auto* cont = reinterpret_cast<SchemeContinuation*>(header);
         if (cont->frames_ptr) gc_trace_continuation_frames(cont);
         for (auto& wf : cont->wind_stack)
            { shade_gray_value(wf.before); shade_gray_value(wf.after); }
         for (auto& v : cont->arg_stack_snapshot) shade_gray_value(v);
         break;
         }
      case GcType::Vector:
         {
         auto* vec = reinterpret_cast<SchemeVector*>(header);
         for (auto& e : vec->elements) shade_gray_value(e);
         break;
         }
      case GcType::Bytevector: break;
      case GcType::Environment:
         {
         auto* env = reinterpret_cast<Environment*>(header);
         for (auto& [sid, val] : env->bindings_) shade_gray_value(val);
         if (env->parent) shade_gray(&env->parent->header);
         break;
         }
      case GcType::Promise:
         shade_gray_value(reinterpret_cast<SchemePromise*>(header)->val);
         break;
      case GcType::Parameter:
         {
         auto* p = reinterpret_cast<SchemeParameter*>(header);
         shade_gray_value(p->current);
         shade_gray_value(p->converter);
         break;
         }
      case GcType::Port:       break;
      case GcType::Complex:    break;
      case GcType::Rational:   break;
      case GcType::MultiValues:
         {
         auto* mv = reinterpret_cast<SchemeMultiValues*>(header);
         for (auto& v : mv->values) shade_gray_value(v);
         break;
         }
      case GcType::Record:
         {
         auto* rec = reinterpret_cast<SchemeRecord*>(header);
         for (auto& f : rec->fields) shade_gray_value(f);
         break;
         }
      case GcType::ExactComplex:
         {
         auto* ec = reinterpret_cast<SchemeExactComplex*>(header);
         shade_gray_value(ec->real);
         shade_gray_value(ec->imag);
         break;
         }
      case GcType::ErrorObject:
         shade_gray_value(reinterpret_cast<SchemeErrorObject*>(header)->irritants);
         break;
      }
   }

// ── Marking ───────────────────────────────────────────────────────────────────

static void mark_environment(Environment* env, bool minor_only);
static void mark_object(GcHeader* header, bool minor_only);

static void mark_value(Value val, bool minor_only)
   {
   GcHeader* h = gc_value_header(val);
   if (!h) return;
   if (minor_only && h->gen == 1) return;
   mark_object(h, minor_only);
   }

static void mark_environment(Environment* env, bool minor_only)
   {
   if (!env) return;
   if (minor_only && env->header.gen == 1) return;
   if (env->header.marked) return;
   env->header.marked = true;
   for (auto& [sid, val] : env->bindings_)
      mark_value(val, minor_only);
   if (env->parent) mark_environment(env->parent, minor_only);
   }

static void mark_object(GcHeader* header, bool minor_only)
   {
   if (!header || header->marked) return;
   if (minor_only && header->gen == 1) return;
   header->marked = true;

   switch (header->type)
      {
      case GcType::Cons:
         {
         auto* cell = reinterpret_cast<ConsCell*>(header);
         mark_value(cell->car, minor_only);
         mark_value(cell->cdr, minor_only);
         break;
         }
      case GcType::String:    break;
      case GcType::Closure:
         {
         auto* cl = reinterpret_cast<SchemeClosure*>(header);
         mark_value(cl->body, minor_only);
         if (cl->env) mark_environment(cl->env, minor_only);
         break;
         }
      case GcType::CaseClosure:
         {
         auto* cc = reinterpret_cast<SchemeCaseClosure*>(header);
         for (auto& clause : cc->clauses) mark_value(clause.body, minor_only);
         if (cc->env) mark_environment(cc->env, minor_only);
         break;
         }
      case GcType::Macro:
         {
         auto* m = reinterpret_cast<SchemeMacro*>(header);
         mark_value(m->body, minor_only);
         if (m->env) mark_environment(m->env, minor_only);
         break;
         }
      case GcType::SyntaxTransformer:
         {
         auto* t = reinterpret_cast<SchemeSyntaxTransformer*>(header);
         for (auto& rule : t->rules)
            { mark_value(rule.pattern, minor_only); mark_value(rule.tmpl, minor_only); }
         if (t->def_env) mark_environment(t->def_env, minor_only);
         break;
         }
      case GcType::Continuation:
         {
         auto* cont = reinterpret_cast<SchemeContinuation*>(header);
         if (cont->frames_ptr) gc_trace_continuation_frames(cont);
         break;
         }
      case GcType::Vector:
         {
         auto* vec = reinterpret_cast<SchemeVector*>(header);
         for (auto& e : vec->elements) mark_value(e, minor_only);
         break;
         }
      case GcType::Bytevector: break;
      case GcType::Environment:
         mark_environment(reinterpret_cast<Environment*>(header), minor_only);
         break;
      case GcType::Promise:
         mark_value(reinterpret_cast<SchemePromise*>(header)->val, minor_only);
         break;
      case GcType::Parameter:
         {
         auto* p = reinterpret_cast<SchemeParameter*>(header);
         mark_value(p->current,   minor_only);
         mark_value(p->converter, minor_only);
         break;
         }
      case GcType::Port:       break;
      case GcType::Complex:    break;
      case GcType::Rational:   break;
      case GcType::MultiValues:
         {
         auto* mv = reinterpret_cast<SchemeMultiValues*>(header);
         for (auto& v : mv->values) mark_value(v, minor_only);
         break;
         }
      case GcType::Record:
         {
         auto* rec = reinterpret_cast<SchemeRecord*>(header);
         for (auto& f : rec->fields) mark_value(f, minor_only);
         break;
         }
      case GcType::ExactComplex:
         {
         auto* ec = reinterpret_cast<SchemeExactComplex*>(header);
         mark_value(ec->real, minor_only);
         mark_value(ec->imag, minor_only);
         break;
         }
      case GcType::ErrorObject:
         mark_value(reinterpret_cast<SchemeErrorObject*>(header)->irritants, minor_only);
         break;
      }
   }

static void mark_roots(bool minor_only)
   {
   for (Value* slot : g_value_roots)
      if (slot) mark_value(*slot, minor_only);
   for (Environment** slot : g_env_roots)
      if (slot && *slot) mark_environment(*slot, minor_only);
   for (auto& hook : g_trace_hooks) hook();
   }

// ── Forward helpers ───────────────────────────────────────────────────────────

static void forward_object_value_fields(GcHeader* header)
   {
   switch (header->type)
      {
      case GcType::Cons:
         {
         auto* cell = reinterpret_cast<ConsCell*>(header);
         gc_forward_value(cell->car);
         gc_forward_value(cell->cdr);
         break;
         }
      case GcType::String:    break;
      case GcType::Closure:
         {
         auto* cl = reinterpret_cast<SchemeClosure*>(header);
         gc_forward_value(cl->body);
         break;
         }
      case GcType::CaseClosure:
         {
         auto* cc = reinterpret_cast<SchemeCaseClosure*>(header);
         for (auto& clause : cc->clauses) gc_forward_value(clause.body);
         break;
         }
      case GcType::Macro:
         {
         auto* m = reinterpret_cast<SchemeMacro*>(header);
         gc_forward_value(m->body);
         break;
         }
      case GcType::SyntaxTransformer:
         {
         auto* t = reinterpret_cast<SchemeSyntaxTransformer*>(header);
         for (auto& rule : t->rules)
            { gc_forward_value(rule.pattern); gc_forward_value(rule.tmpl); }
         break;
         }
      case GcType::Continuation:
         {
         auto* cont = reinterpret_cast<SchemeContinuation*>(header);
         if (cont->frames_ptr) gc_forward_continuation_frames(cont);
         for (auto& wf : cont->wind_stack)
            { gc_forward_value(wf.before); gc_forward_value(wf.after); }
         for (auto& v : cont->arg_stack_snapshot) gc_forward_value(v);
         break;
         }
      case GcType::Vector:
         {
         auto* vec = reinterpret_cast<SchemeVector*>(header);
         for (auto& e : vec->elements) gc_forward_value(e);
         break;
         }
      case GcType::Bytevector: break;
      case GcType::Environment:
         {
         auto* env = reinterpret_cast<Environment*>(header);
         for (auto& [sid, val] : env->bindings_)
            gc_forward_value(val);
         break;
         }
      case GcType::Promise:
         gc_forward_value(reinterpret_cast<SchemePromise*>(header)->val);
         break;
      case GcType::Parameter:
         {
         auto* p = reinterpret_cast<SchemeParameter*>(header);
         gc_forward_value(p->current);
         gc_forward_value(p->converter);
         break;
         }
      case GcType::Port:       break;
      case GcType::Complex:    break;
      case GcType::Rational:   break;
      case GcType::MultiValues:
         {
         auto* mv = reinterpret_cast<SchemeMultiValues*>(header);
         for (auto& v : mv->values) gc_forward_value(v);
         break;
         }
      case GcType::Record:
         {
         auto* rec = reinterpret_cast<SchemeRecord*>(header);
         for (auto& f : rec->fields) gc_forward_value(f);
         break;
         }
      case GcType::ExactComplex:
         {
         auto* ec = reinterpret_cast<SchemeExactComplex*>(header);
         gc_forward_value(ec->real);
         gc_forward_value(ec->imag);
         break;
         }
      case GcType::ErrorObject:
         gc_forward_value(reinterpret_cast<SchemeErrorObject*>(header)->irritants);
         break;
      }
   }

// ── Sweep helpers ─────────────────────────────────────────────────────────────

static void free_object(GcHeader* header)
   {
   switch (header->type)
      {
      case GcType::Cons:            delete reinterpret_cast<ConsCell*>(header);               break;
      case GcType::String:          delete reinterpret_cast<SchemeString*>(header);           break;
      case GcType::Closure:         delete reinterpret_cast<SchemeClosure*>(header);          break;
      case GcType::CaseClosure:     delete reinterpret_cast<SchemeCaseClosure*>(header);      break;
      case GcType::Macro:           delete reinterpret_cast<SchemeMacro*>(header);            break;
      case GcType::SyntaxTransformer: delete reinterpret_cast<SchemeSyntaxTransformer*>(header); break;
      case GcType::Continuation:    delete reinterpret_cast<SchemeContinuation*>(header);     break;
      case GcType::Vector:          delete reinterpret_cast<SchemeVector*>(header);           break;
      case GcType::Bytevector:      delete reinterpret_cast<SchemeBytevector*>(header);       break;
      case GcType::Environment:     delete reinterpret_cast<Environment*>(header);            break;
      case GcType::Promise:         delete reinterpret_cast<SchemePromise*>(header);          break;
      case GcType::Parameter:       delete reinterpret_cast<SchemeParameter*>(header);        break;
      case GcType::Port:            delete reinterpret_cast<SchemePort*>(header);             break;
      case GcType::Complex:         delete reinterpret_cast<SchemeComplex*>(header);          break;
      case GcType::Rational:        delete reinterpret_cast<SchemeRational*>(header);         break;
      case GcType::MultiValues:     delete reinterpret_cast<SchemeMultiValues*>(header);      break;
      case GcType::Record:          delete reinterpret_cast<SchemeRecord*>(header);           break;
      case GcType::ExactComplex:    delete reinterpret_cast<SchemeExactComplex*>(header);     break;
      case GcType::ErrorObject:     delete reinterpret_cast<SchemeErrorObject*>(header);      break;
      }
   }

// ── Trace remembered set children ────────────────────────────────────────────

static void trace_remembered_children(GcHeader* header)
   {
   switch (header->type)
      {
      case GcType::Cons:
         {
         auto* cell = reinterpret_cast<ConsCell*>(header);
         mark_value(cell->car, true);
         mark_value(cell->cdr, true);
         break;
         }
      case GcType::String:    break;
      case GcType::Closure:
         {
         auto* cl = reinterpret_cast<SchemeClosure*>(header);
         mark_value(cl->body, true);
         if (cl->env) mark_environment(cl->env, true);
         break;
         }
      case GcType::CaseClosure:
         {
         auto* cc = reinterpret_cast<SchemeCaseClosure*>(header);
         for (auto& clause : cc->clauses) mark_value(clause.body, true);
         if (cc->env) mark_environment(cc->env, true);
         break;
         }
      case GcType::Macro:
         {
         auto* m = reinterpret_cast<SchemeMacro*>(header);
         mark_value(m->body, true);
         if (m->env) mark_environment(m->env, true);
         break;
         }
      case GcType::SyntaxTransformer:
         {
         auto* t = reinterpret_cast<SchemeSyntaxTransformer*>(header);
         for (auto& rule : t->rules)
            { mark_value(rule.pattern, true); mark_value(rule.tmpl, true); }
         if (t->def_env) mark_environment(t->def_env, true);
         break;
         }
      case GcType::Continuation:
         {
         auto* cont = reinterpret_cast<SchemeContinuation*>(header);
         if (cont->frames_ptr) gc_trace_continuation_frames(cont);
         break;
         }
      case GcType::Vector:
         {
         auto* vec = reinterpret_cast<SchemeVector*>(header);
         for (auto& e : vec->elements) mark_value(e, true);
         break;
         }
      case GcType::Bytevector: break;
      case GcType::Environment:
         {
         auto* env = reinterpret_cast<Environment*>(header);
         for (auto& [sid, val] : env->bindings_) mark_value(val, true);
         if (env->parent) mark_environment(env->parent, true);
         break;
         }
      case GcType::Promise:
         mark_value(reinterpret_cast<SchemePromise*>(header)->val, true);
         break;
      case GcType::Parameter:
         {
         auto* p = reinterpret_cast<SchemeParameter*>(header);
         mark_value(p->current,   true);
         mark_value(p->converter, true);
         break;
         }
      case GcType::Port:       break;
      case GcType::Complex:    break;
      case GcType::Rational:   break;
      case GcType::MultiValues:
         {
         auto* mv = reinterpret_cast<SchemeMultiValues*>(header);
         for (auto& v : mv->values) mark_value(v, true);
         break;
         }
      case GcType::Record:
         {
         auto* rec = reinterpret_cast<SchemeRecord*>(header);
         for (auto& f : rec->fields) mark_value(f, true);
         break;
         }
      case GcType::ExactComplex:
         {
         auto* ec = reinterpret_cast<SchemeExactComplex*>(header);
         mark_value(ec->real, true);
         mark_value(ec->imag, true);
         break;
         }
      case GcType::ErrorObject:
         mark_value(reinterpret_cast<SchemeErrorObject*>(header)->irritants, true);
         break;
      }
   }

// ── Nursery evacuation ────────────────────────────────────────────────────────

static void nursery_evacuate_and_forward(
   const std::vector<GcHeader*>& freshly_promoted)
   {
   if (g_nursery_bump == 0) return;

   std::vector<ConsCell*> evacuated;
   evacuated.reserve(g_nursery_bump);

   for (size_t i = 0; i < g_nursery_bump; ++i)
      {
      ConsCell* old_cell = &g_nursery[i];
      if (!old_cell->header.marked) continue;

      auto* new_cell         = new ConsCell{};
      new_cell->car          = old_cell->car;
      new_cell->cdr          = old_cell->cdr;
      new_cell->header.gen   = 1;
      new_cell->header.next  = g_old_head;
      g_old_head             = &new_cell->header;
      ++g_old_count;
      old_cell->header.forward = &new_cell->header;

      GcPhase cur_phase = g_gc_phase.load(std::memory_order_relaxed);
      if (cur_phase == GcPhase::Marking)
         shade_gray(&new_cell->header);
      else if (cur_phase == GcPhase::Sweeping)
         new_cell->header.marked.store(true, std::memory_order_relaxed);

      evacuated.push_back(new_cell);
      }

   for (Value* slot : g_value_roots)  gc_forward_value(*slot);
   for (auto& hook : g_forward_hooks) hook();
   for (GcHeader* obj : g_remembered_set) forward_object_value_fields(obj);
   for (GcHeader* obj : freshly_promoted) forward_object_value_fields(obj);
   for (ConsCell* cell : evacuated)
      { gc_forward_value(cell->car); gc_forward_value(cell->cdr); }
   for (GcHeader*& entry : g_gray_stack)
      if (entry && entry->forward) entry = entry->forward;

   g_nursery_bump = 0;
   }

// ── Minor GC ─────────────────────────────────────────────────────────────────

static void minor_collect()
   {
   g_minor_gc_active = true;
   mark_roots(true);
   for (GcHeader* old_obj : g_remembered_set)
      trace_remembered_children(old_obj);

   std::vector<GcHeader*> freshly_promoted;
   {
   GcHeader** prev = &g_young_head;
   while (*prev)
      {
      GcHeader* obj = *prev;
      if (obj->marked)
         {
         obj->marked = false;
         obj->gen    = 1;
         *prev       = obj->next;
         obj->next   = g_old_head;
         g_old_head  = obj;
         ++g_old_count;
         --g_young_count;
         freshly_promoted.push_back(obj);
         if (g_gc_phase.load(std::memory_order_relaxed) == GcPhase::Sweeping)
            obj->marked.store(true, std::memory_order_relaxed);
         }
      else
         {
         *prev = obj->next;
         free_object(obj);
         --g_young_count;
         }
      }
   }

   nursery_evacuate_and_forward(freshly_promoted);
   g_minor_gc_active = false;
   g_remembered_set.clear();
   }

// ── Incremental major GC ──────────────────────────────────────────────────────

static void gc_major_start()
   {
   g_gc_thread_stop.store(false, std::memory_order_relaxed);
   g_gray_inflight.store(0, std::memory_order_relaxed);
   g_gc_phase.store(GcPhase::Marking, std::memory_order_release);

   for (Value* slot : g_value_roots)  shade_gray_value(*slot);
   for (Environment** slot : g_env_roots)
      if (*slot) shade_gray(&(*slot)->header);
   for (auto& hook : g_trace_hooks) hook();

   g_gc_thread = std::thread(gc_marking_thread_fn);
   }

static void gc_major_step()
   {
   if (g_old_threshold > 0 && g_old_count >= g_old_threshold * 4)
      { gc_major_finish(); return; }

   for (Value* slot : g_value_roots)  shade_gray_value(*slot);
   for (Environment** slot : g_env_roots)
      if (*slot) shade_gray(&(*slot)->header);
   for (auto& hook : g_trace_hooks) hook();

   if (g_gray_inflight.load(std::memory_order_acquire) == 0)
      gc_major_finish();
   }

static void gc_major_finish()
   {
   g_gc_thread_stop.store(true, std::memory_order_release);
   g_mark_cv.notify_all();
   if (g_gc_thread.joinable()) g_gc_thread.join();

   g_gc_phase.store(GcPhase::Sweeping, std::memory_order_release);
   minor_collect();

   while (!g_gray_stack.empty())
      {
      GcHeader* obj = g_gray_stack.back();
      g_gray_stack.pop_back();
      process_gray_object(obj);
      }
   g_gray_inflight.store(0, std::memory_order_relaxed);

   g_sweep_cursor = &g_old_head;
   }

static void gc_sweep_step()
   {
   size_t processed = 0;
   while (g_sweep_cursor && *g_sweep_cursor && processed < SWEEP_STEP_BUDGET)
      {
      GcHeader* obj = *g_sweep_cursor;
      if (obj->marked.load(std::memory_order_relaxed))
         { obj->marked.store(false, std::memory_order_relaxed); g_sweep_cursor = &obj->next; }
      else
         { *g_sweep_cursor = obj->next; free_object(obj); --g_old_count; }
      ++processed;
      }
   if (!g_sweep_cursor || !*g_sweep_cursor)
      {
      g_sweep_cursor = nullptr;
      g_remembered_set.clear();
      g_gc_phase.store(GcPhase::Idle, std::memory_order_release);
      if (g_old_threshold > 0 && g_old_count >= g_old_threshold)
         g_old_threshold = g_old_count * 2;
      }
   }

// ── Public collection entry point ─────────────────────────────────────────────

bool gc_needs_collection()
   {
   GcPhase phase = g_gc_phase.load(std::memory_order_relaxed);
   return (g_nursery_bump >= NURSERY_CAPACITY)
       || (g_young_threshold > 0 && g_young_count >= g_young_threshold)
       || (g_old_threshold   > 0 && g_old_count   >= g_old_threshold)
       || (phase == GcPhase::Marking)
       || (phase == GcPhase::Sweeping);
   }

void gc_collect()
   {
   GcPhase phase = g_gc_phase.load(std::memory_order_acquire);
   if (phase == GcPhase::Sweeping)
      {
      if (g_nursery_bump >= NURSERY_CAPACITY
            || (g_young_threshold > 0 && g_young_count >= g_young_threshold))
         minor_collect();
      gc_sweep_step();
      return;
      }
   if (phase == GcPhase::Idle)
      {
      if (g_nursery_bump >= NURSERY_CAPACITY
            || (g_young_threshold > 0 && g_young_count >= g_young_threshold))
         minor_collect();
      }
   if (phase == GcPhase::Idle
         && g_old_threshold > 0 && g_old_count >= g_old_threshold)
      gc_major_start();
   if (g_gc_phase.load(std::memory_order_relaxed) == GcPhase::Marking)
      gc_major_step();
   }

// ── Public trace wrappers ─────────────────────────────────────────────────────

void gc_trace_value(Value val)
   {
   if (g_gc_phase.load(std::memory_order_relaxed) == GcPhase::Marking && !g_minor_gc_active)
      shade_gray_value(val);
   else
      mark_value(val, g_minor_gc_active);
   }

void gc_trace_environment(Environment* env)
   {
   if (g_gc_phase.load(std::memory_order_relaxed) == GcPhase::Marking && !g_minor_gc_active)
      { if (env) shade_gray(&env->header); }
   else
      mark_environment(env, g_minor_gc_active);
   }

// ── Allocation ────────────────────────────────────────────────────────────────

static void register_young(GcHeader* header)
   {
   header->gen  = 0;
   header->next = g_young_head;
   g_young_head = header;
   ++g_young_count;
   }

ConsCell* gc_alloc_cons()
   {
   if (g_nursery_bump < NURSERY_CAPACITY)
      {
      ConsCell* cell       = &g_nursery[g_nursery_bump++];
      cell->header.marked  = false;
      cell->header.gen     = 0;
      cell->header.type    = GcType::Cons;
      cell->header.next    = nullptr;
      cell->header.forward = nullptr;
      cell->car            = Value{};
      cell->cdr            = Value{};
      return cell;
      }
   auto* cell = new ConsCell{};
   register_young(&cell->header);
   return cell;
   }

SchemeString*  gc_alloc_string(const std::string& s)
   { auto* p = new SchemeString{s}; register_young(&p->header); return p; }

SchemeClosure* gc_alloc_closure()
   { auto* p = new SchemeClosure{}; register_young(&p->header); return p; }

SchemeMacro*   gc_alloc_macro()
   { auto* p = new SchemeMacro{}; register_young(&p->header); return p; }

SchemeSyntaxTransformer* gc_alloc_syntax_transformer()
   { auto* p = new SchemeSyntaxTransformer{}; register_young(&p->header); return p; }

SchemeContinuation* gc_alloc_continuation()
   { auto* p = new SchemeContinuation{}; register_young(&p->header); return p; }

SchemeVector* gc_alloc_vector(size_t n, Value fill)
   { auto* p = new SchemeVector{n, fill}; register_young(&p->header); return p; }

SchemeBytevector* gc_alloc_bytevector(size_t n, uint8_t fill)
   { auto* p = new SchemeBytevector{n, fill}; register_young(&p->header); return p; }

Environment* gc_alloc_environment(Environment* parent)
   { auto* p = new Environment{parent}; register_young(&p->header); return p; }

SchemePromise* gc_alloc_promise(Value thunk)
   { auto* p = new SchemePromise{}; p->val = thunk; register_young(&p->header); return p; }

SchemeParameter* gc_alloc_parameter(Value init, Value converter)
   {
   auto* p = new SchemeParameter{};
   p->current = init; p->converter = converter;
   register_young(&p->header); return p;
   }

SchemePort* gc_alloc_port()
   { auto* p = new SchemePort{}; register_young(&p->header); return p; }

SchemeComplex* gc_alloc_complex(double r, double i)
   { auto* p = new SchemeComplex{r, i}; register_young(&p->header); return p; }

SchemeRational* gc_alloc_rational(int64_t n, int64_t d)
   { auto* p = new SchemeRational{n, d}; register_young(&p->header); return p; }

SchemeCaseClosure* gc_alloc_case_closure()
   { auto* p = new SchemeCaseClosure{}; register_young(&p->header); return p; }

SchemeMultiValues* gc_alloc_multi_values()
   { auto* p = new SchemeMultiValues{}; register_young(&p->header); return p; }

SchemeRecord* gc_alloc_record()
   { auto* p = new SchemeRecord{}; register_young(&p->header); return p; }

SchemeExactComplex* gc_alloc_exact_complex(Value real_part, Value imag_part)
   {
   auto* p = new SchemeExactComplex{};
   p->real = real_part; p->imag = imag_part;
   register_young(&p->header); return p;
   }

SchemeErrorObject* gc_alloc_error_object()
   { auto* p = new SchemeErrorObject{}; register_young(&p->header); return p; }

// ── Root tracking ─────────────────────────────────────────────────────────────

void gc_root_push(Value* slot) { g_value_roots.push_back(slot); }
void gc_root_pop(Value* slot)
   {
   for (auto it = g_value_roots.rbegin(); it != g_value_roots.rend(); ++it)
      if (*it == slot) { g_value_roots.erase(std::next(it).base()); return; }
   }

void gc_env_root_push(Environment** slot) { g_env_roots.push_back(slot); }
void gc_env_root_pop(Environment** slot)
   {
   for (auto it = g_env_roots.rbegin(); it != g_env_roots.rend(); ++it)
      if (*it == slot) { g_env_roots.erase(std::next(it).base()); return; }
   }

void gc_push_trace_hook(GcTraceHook hook)     { g_trace_hooks.push_back(std::move(hook)); }
void gc_pop_trace_hook()
   { if (!g_trace_hooks.empty()) g_trace_hooks.pop_back(); }

void gc_push_forward_hook(GcForwardHook hook) { g_forward_hooks.push_back(std::move(hook)); }
void gc_pop_forward_hook()
   { if (!g_forward_hooks.empty()) g_forward_hooks.pop_back(); }

// ── Threshold / stats ─────────────────────────────────────────────────────────

void gc_set_threshold(size_t t)
   {
   g_old_threshold   = t;
   g_young_threshold = (t >= 4) ? t / 4 : t;
   }

size_t gc_object_count()
   { return g_nursery_bump + g_young_count + g_old_count; }

// ── Init / shutdown ───────────────────────────────────────────────────────────

void gc_init()
   {
   g_nursery_bump    = 0;
   g_young_head = g_old_head = nullptr;
   g_young_count = g_old_count = 0;
   g_young_threshold = 256;
   g_old_threshold   = 1024;
   g_gc_phase.store(GcPhase::Idle,  std::memory_order_relaxed);
   g_gc_thread_stop.store(false,     std::memory_order_relaxed);
   g_gray_inflight.store(0,          std::memory_order_relaxed);
   g_sweep_cursor = nullptr;
   g_gray_stack.clear();
   g_remembered_set.clear();
   g_value_roots.clear();
   g_env_roots.clear();
   g_trace_hooks.clear();
   g_forward_hooks.clear();
   }

void gc_shutdown()
   {
   if (g_gc_thread.joinable())
      {
      g_gc_thread_stop.store(true, std::memory_order_release);
      g_mark_cv.notify_all();
      g_gc_thread.join();
      }
   g_value_roots.clear();
   g_env_roots.clear();
   g_remembered_set.clear();

   auto free_list = [](GcHeader* head)
      {
      GcHeader* cur = head;
      while (cur) { GcHeader* nxt = cur->next; free_object(cur); cur = nxt; }
      };

   g_nursery_bump = 0;
   g_gc_phase.store(GcPhase::Idle, std::memory_order_relaxed);
   g_sweep_cursor = nullptr;
   g_gray_stack.clear();
   g_gray_inflight.store(0, std::memory_order_relaxed);

   free_list(g_young_head);
   free_list(g_old_head);
   g_young_head = g_old_head = nullptr;
   g_young_count = g_old_count = 0;
   }
