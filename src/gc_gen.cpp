// gc_gen.cpp -- Generational GC: bump-pointer nursery + concurrent tri-color major GC.
// Adapted from CPPScheme's gc_gen.cpp for CPPScheme2's type set.
// Environment internals are opaque here; tracing/forwarding delegated to
// gc_trace_environment_children / gc_forward_environment_children (Environment.cpp).
// Continuation frame tracing/forwarding delegated to gc_trace/forward_continuation_frames
// (Evaluator.cpp).

#include "gc.h"
#include "AST.h"
#include "Environment.h"
#include "profiler.h"
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cassert>
#include <cstdio>

// ── Nursery ───────────────────────────────────────────────────────────────────
// Fixed slab of ConsCells allocated via bump pointer.
// Not on g_young_head and not counted in g_young_count.

static constexpr size_t NURSERY_CAPACITY = 65536;
static ConsCell  g_nursery[NURSERY_CAPACITY];
static size_t    g_nursery_bump = 0;

// ── State ─────────────────────────────────────────────────────────────────────

static GcHeader* g_young_head      = nullptr;
static size_t    g_young_count     = 0;
static size_t    g_young_threshold = 16384;

static GcHeader* g_old_head        = nullptr;
static size_t    g_old_count       = 0;
static size_t    g_old_threshold   = 131072;

// ── GC-stress mode (the ]gc-stress listener command) ──────────────────────────
// When enabled, the collection thresholds and the *effective* nursery limit are
// slashed so minor collections fire constantly -- exercising the moving GC (and
// surfacing any missing-root bug) on whatever workload is running, with no
// recompile.  GC is invisible to Scheme semantics, so results are unchanged; the
// run just gets much slower and much more thorough.  Toggled at runtime; the
// pre-stress thresholds are saved so they can be restored on disable.
static bool   g_gc_stress             = false;
static size_t g_nursery_limit         = NURSERY_CAPACITY;  // collect when bump hits this
static size_t g_saved_young_threshold = 0;
static size_t g_saved_old_threshold   = 0;

static std::unordered_set<GcHeader*> g_remembered_set;
static bool g_minor_gc_active = false;

static std::vector<Value*>               g_value_roots;
static std::vector<Environment**>        g_env_roots;
static std::vector<std::vector<Value>*>  g_vec_roots;
static std::vector<GcTraceHook>   g_trace_hooks;
static std::vector<GcForwardHook> g_forward_hooks;

// ── Incremental major GC state ────────────────────────────────────────────────
// Marking runs incrementally on the main thread (no background thread) to avoid
// data races between the marker's env iteration and the evaluator's env writes.

enum class GcPhase { Idle, Marking, Sweeping };
static GcPhase                 g_gc_phase = GcPhase::Idle;

static std::vector<GcHeader*>  g_gray_stack;
static constexpr size_t        MARK_STEP_BUDGET  = 512;

static GcHeader**              g_sweep_cursor    = nullptr;
static constexpr size_t        SWEEP_STEP_BUDGET = 512;

// Objects promoted to old-gen while a sweep is in progress get marked=true to
// avoid being immediately freed by that sweep.  We track them here so that when
// the sweep finishes (gc_sweep_step) we can clear those stale marks before the
// phase returns to Idle — otherwise check_invariants would see marked objects in
// Idle phase, and shade_gray() would skip them in the next major GC.
static std::vector<GcHeader*>  g_promoted_during_sweep;

// ── Forward declarations ──────────────────────────────────────────────────────
static void shade_gray(GcHeader* header);
static void process_gray_object(GcHeader*);
static void gc_major_finish();
static void minor_collect();
static void mark_environment(Environment* env, bool minor_only);

// ── GC registration helper (exported for gc_alloc_environment in Environment.cpp) ──

static void register_young(GcHeader* header) {
    header->gen  = 0;
    header->next = g_young_head;
    g_young_head = header;
    ++g_young_count;
}

void gc_register_young(GcHeader* header) { register_young(header); }

// ── Write barrier ─────────────────────────────────────────────────────────────

void gc_write_barrier(GcHeader* parent, GcHeader* child) {
    if (!parent || !child) return;
    if (parent->gen == 1 && child->gen == 0)
        g_remembered_set.insert(parent);
    if (g_gc_phase == GcPhase::Marking
            && parent->marked.load(std::memory_order_relaxed)
            && child->gen == 1)
        shade_gray(child);
}

// ── Pointer forwarding ────────────────────────────────────────────────────────

void gc_copy_forward_value(Value& val)  { gc_forward_value(val); }
void gc_copy_forward_env(Environment*& env_ptr) {
    if (!env_ptr) return;
    GcHeader* hdr = reinterpret_cast<GcHeader*>(env_ptr);
    if (hdr->forward)
        env_ptr = reinterpret_cast<Environment*>(hdr->forward);
}

// ── Shade helpers ─────────────────────────────────────────────────────────────

static void shade_gray(GcHeader* header) {
    if (!header || header->gen != 1) return;
    if (header->marked.load(std::memory_order_relaxed)) return;
    header->marked.store(true, std::memory_order_relaxed);
    g_gray_stack.push_back(header);
}

static void shade_gray_value(Value val) { shade_gray(gc_value_header(val)); }

static void shade_gray_env(Environment* env) {
    if (env) shade_gray(reinterpret_cast<GcHeader*>(env));
}

// ── Incremental marking ───────────────────────────────────────────────────────
// process_gray_object: shade each directly reachable old-gen child gray.

static void process_gray_object(GcHeader* header) {
    switch (header->type) {
    case GcType::Cons: {
        auto* c = reinterpret_cast<ConsCell*>(header);
        shade_gray_value(c->car);
        shade_gray_value(c->cdr);
        break;
    }
    case GcType::String:
        break;
    case GcType::Closure: {
        auto* cl = reinterpret_cast<SchemeClosure*>(header);
        shade_gray_value(cl->body);
        shade_gray_env(cl->env);
        break;
    }
    case GcType::CaseClosure: {
        auto* cc = reinterpret_cast<CaseClosure*>(header);
        for (auto& clause : cc->clauses)
            shade_gray_value(clause.body);
        shade_gray_env(cc->env);
        break;
    }
    case GcType::Promise:
        shade_gray_value(reinterpret_cast<Promise*>(header)->payload);
        break;
    case GcType::MultiValues: {
        auto* mv = reinterpret_cast<MultiValues*>(header);
        for (auto& v : mv->values)
            shade_gray_value(v);
        break;
    }
    case GcType::Record: {
        auto* rec = reinterpret_cast<Record*>(header);
        for (auto& fv : rec->field_values)
            shade_gray_value(fv);
        if (rec->record_type) shade_gray(&rec->record_type->header);
        break;
    }
    case GcType::RecordType:
        break;
    case GcType::Parameter: {
        auto* p = reinterpret_cast<Parameter*>(header);
        shade_gray_value(p->value);
        shade_gray_value(p->converter);
        break;
    }
    case GcType::ErrorObject: {
        auto* eo = reinterpret_cast<ErrorObject*>(header);
        for (auto& irr : eo->irritants)
            shade_gray_value(irr);
        break;
    }
    case GcType::Continuation: {
        auto* k = reinterpret_cast<Continuation*>(header);
        gc_trace_continuation_frames(k);
        for (auto& wf : k->wind_snapshot) {
            shade_gray_value(wf.before);
            shade_gray_value(wf.after);
        }
        for (auto& v : k->handler_snapshot) shade_gray_value(v);
        for (auto& v : k->shadow_snapshot)  shade_gray_value(v);
        break;
    }
    case GcType::SyntaxTransformer: {
        auto* st = reinterpret_cast<SyntaxTransformer*>(header);
        for (auto& rule : st->rules) {
            shade_gray_value(rule.pattern);
            shade_gray_value(rule.tmpl);
        }
        break;
    }
    case GcType::Vector: {
        auto* v = reinterpret_cast<SchemeVector*>(header);
        for (auto& elem : v->elements)
            shade_gray_value(elem);
        break;
    }
    case GcType::Bytevector:
    case GcType::Port:
    case GcType::Complex:
    case GcType::Rational:
    case GcType::Bignum:
    case GcType::Integer:
    case GcType::Real:
    case GcType::Char:
        break;
    case GcType::ExactComplex: {
        auto* ec = reinterpret_cast<ExactComplex*>(header);
        shade_gray_value(ec->re);
        shade_gray_value(ec->im);
        break;
    }
    case GcType::RecordAccessor:
        if (reinterpret_cast<RecordAccessor*>(header)->record_type)
            shade_gray(&reinterpret_cast<RecordAccessor*>(header)->record_type->header);
        break;
    case GcType::RecordMutator:
        if (reinterpret_cast<RecordMutator*>(header)->record_type)
            shade_gray(&reinterpret_cast<RecordMutator*>(header)->record_type->header);
        break;
    case GcType::Environment:
        gc_trace_environment_children(
            reinterpret_cast<Environment*>(header), /*minor_only=*/false);
        break;
    case GcType::EnvBox:
        if (reinterpret_cast<EnvBox*>(header)->env)
            shade_gray(&reinterpret_cast<EnvBox*>(header)->env->header);
        break;
    }
}

// ── Minor/major marking ───────────────────────────────────────────────────────

static void mark_value(Value val, bool minor_only);
static void mark_object(GcHeader* header, bool minor_only);

static void mark_value(Value val, bool minor_only) {
    GcHeader* header = gc_value_header(val);
    if (!header) return;
    if (minor_only && header->gen == 1) return;
    mark_object(header, minor_only);
}

static void mark_environment(Environment* env, bool minor_only) {
    if (!env) return;
    GcHeader* hdr = reinterpret_cast<GcHeader*>(env);
    if (minor_only && hdr->gen == 1) return;
    if (hdr->marked.load(std::memory_order_relaxed)) return;
    hdr->marked.store(true, std::memory_order_relaxed);
    gc_trace_environment_children(env, minor_only);
}

static void mark_object(GcHeader* header, bool minor_only) {
    if (!header) return;
    if (minor_only && header->gen == 1) return;
    if (header->marked.load(std::memory_order_relaxed)) return;
    header->marked.store(true, std::memory_order_relaxed);

    // Iteratively follow the cdr chain for cons cells to avoid O(N) C++ stack
    // depth on long linked lists.  car is always handled with the normal
    // (potentially recursive) mark_value call, which is fine because car values
    // are typically leaves (symbols, integers) or short structures.
    while (header->type == GcType::Cons) {
        auto* c = reinterpret_cast<ConsCell*>(header);
        mark_value(c->car, minor_only);
        GcHeader* next = gc_value_header(c->cdr);
        if (!next) return;
        if (minor_only && next->gen == 1) return;
        if (next->marked.load(std::memory_order_relaxed)) return;
        next->marked.store(true, std::memory_order_relaxed);
        header = next;
    }

    switch (header->type) {
    case GcType::Cons:
        // unreachable: handled by the loop above
        break;
    case GcType::String:
        break;
    case GcType::Closure: {
        auto* cl = reinterpret_cast<SchemeClosure*>(header);
        mark_value(cl->body, minor_only);
        mark_environment(cl->env, minor_only);
        break;
    }
    case GcType::CaseClosure: {
        auto* cc = reinterpret_cast<CaseClosure*>(header);
        for (auto& clause : cc->clauses)
            mark_value(clause.body, minor_only);
        mark_environment(cc->env, minor_only);
        break;
    }
    case GcType::Promise:
        mark_value(reinterpret_cast<Promise*>(header)->payload, minor_only);
        break;
    case GcType::MultiValues: {
        auto* mv = reinterpret_cast<MultiValues*>(header);
        for (auto& v : mv->values)
            mark_value(v, minor_only);
        break;
    }
    case GcType::Record: {
        auto* rec = reinterpret_cast<Record*>(header);
        for (auto& fv : rec->field_values)
            mark_value(fv, minor_only);
        if (rec->record_type)
            mark_object(&rec->record_type->header, minor_only);
        break;
    }
    case GcType::RecordType:
        break;
    case GcType::Parameter: {
        auto* p = reinterpret_cast<Parameter*>(header);
        mark_value(p->value,     minor_only);
        mark_value(p->converter, minor_only);
        break;
    }
    case GcType::ErrorObject: {
        auto* eo = reinterpret_cast<ErrorObject*>(header);
        for (auto& irr : eo->irritants)
            mark_value(irr, minor_only);
        break;
    }
    case GcType::Continuation: {
        auto* k = reinterpret_cast<Continuation*>(header);
        gc_trace_continuation_frames(k);
        for (auto& wf : k->wind_snapshot) {
            mark_value(wf.before, minor_only);
            mark_value(wf.after,  minor_only);
        }
        for (auto& v : k->handler_snapshot) mark_value(v, minor_only);
        for (auto& v : k->shadow_snapshot)  mark_value(v, minor_only);
        break;
    }
    case GcType::SyntaxTransformer: {
        auto* st = reinterpret_cast<SyntaxTransformer*>(header);
        for (auto& rule : st->rules) {
            mark_value(rule.pattern, minor_only);
            mark_value(rule.tmpl,    minor_only);
        }
        break;
    }
    case GcType::Vector: {
        auto* v = reinterpret_cast<SchemeVector*>(header);
        for (auto& elem : v->elements)
            mark_value(elem, minor_only);
        break;
    }
    case GcType::Bytevector:
    case GcType::Port:
    case GcType::Complex:
    case GcType::Rational:
    case GcType::Bignum:
    case GcType::Integer:
    case GcType::Real:
    case GcType::Char:
        break;
    case GcType::ExactComplex: {
        auto* ec = reinterpret_cast<ExactComplex*>(header);
        mark_value(ec->re, minor_only);
        mark_value(ec->im, minor_only);
        break;
    }
    case GcType::RecordAccessor:
        if (reinterpret_cast<RecordAccessor*>(header)->record_type)
            mark_object(&reinterpret_cast<RecordAccessor*>(header)->record_type->header, minor_only);
        break;
    case GcType::RecordMutator:
        if (reinterpret_cast<RecordMutator*>(header)->record_type)
            mark_object(&reinterpret_cast<RecordMutator*>(header)->record_type->header, minor_only);
        break;
    case GcType::Environment:
        mark_environment(reinterpret_cast<Environment*>(header), minor_only);
        break;
    case GcType::EnvBox:
        if (reinterpret_cast<EnvBox*>(header)->env)
            mark_environment(reinterpret_cast<EnvBox*>(header)->env, minor_only);
        break;
    }
}

static void mark_roots(bool minor_only) {
    for (Value* slot : g_value_roots)
        if (slot) mark_value(*slot, minor_only);
    for (Environment** slot : g_env_roots)
        if (slot && *slot) mark_environment(*slot, minor_only);
    for (std::vector<Value>* vec : g_vec_roots)
        if (vec) for (Value& v : *vec) mark_value(v, minor_only);
    for (auto& hook : g_trace_hooks)
        hook();
}

// ── Remembered-set child tracing ──────────────────────────────────────────────
// Trace young-gen children of an old-gen object.  Called only during minor GC.

static void trace_remembered_children(GcHeader* header) {
    switch (header->type) {
    case GcType::Cons: {
        auto* c = reinterpret_cast<ConsCell*>(header);
        mark_value(c->car, true);
        mark_value(c->cdr, true);
        break;
    }
    case GcType::String: break;
    case GcType::Closure: {
        auto* cl = reinterpret_cast<SchemeClosure*>(header);
        mark_value(cl->body, true);
        mark_environment(cl->env, true);
        break;
    }
    case GcType::CaseClosure: {
        auto* cc = reinterpret_cast<CaseClosure*>(header);
        for (auto& clause : cc->clauses)
            mark_value(clause.body, true);
        mark_environment(cc->env, true);
        break;
    }
    case GcType::Promise:
        mark_value(reinterpret_cast<Promise*>(header)->payload, true);
        break;
    case GcType::MultiValues: {
        auto* mv = reinterpret_cast<MultiValues*>(header);
        for (auto& v : mv->values) mark_value(v, true);
        break;
    }
    case GcType::Record: {
        auto* rec = reinterpret_cast<Record*>(header);
        for (auto& fv : rec->field_values) mark_value(fv, true);
        if (rec->record_type) mark_object(&rec->record_type->header, true);
        break;
    }
    case GcType::RecordType: break;
    case GcType::Parameter: {
        auto* p = reinterpret_cast<Parameter*>(header);
        mark_value(p->value,     true);
        mark_value(p->converter, true);
        break;
    }
    case GcType::ErrorObject: {
        auto* eo = reinterpret_cast<ErrorObject*>(header);
        for (auto& irr : eo->irritants) mark_value(irr, true);
        break;
    }
    case GcType::Continuation: {
        auto* k = reinterpret_cast<Continuation*>(header);
        gc_trace_continuation_frames(k);
        for (auto& wf : k->wind_snapshot) {
            mark_value(wf.before, true);
            mark_value(wf.after,  true);
        }
        for (auto& v : k->handler_snapshot) mark_value(v, true);
        for (auto& v : k->shadow_snapshot)  mark_value(v, true);
        break;
    }
    case GcType::SyntaxTransformer: {
        auto* st = reinterpret_cast<SyntaxTransformer*>(header);
        for (auto& rule : st->rules) {
            mark_value(rule.pattern, true);
            mark_value(rule.tmpl,    true);
        }
        break;
    }
    case GcType::Vector: {
        auto* v = reinterpret_cast<SchemeVector*>(header);
        for (auto& elem : v->elements) mark_value(elem, true);
        break;
    }
    case GcType::Bytevector:
    case GcType::Port:
    case GcType::Complex:
    case GcType::Rational:
    case GcType::Bignum:
    case GcType::Integer:
    case GcType::Real:
    case GcType::Char:
        break;
    case GcType::ExactComplex: {
        auto* ec = reinterpret_cast<ExactComplex*>(header);
        mark_value(ec->re, true);
        mark_value(ec->im, true);
        break;
    }
    case GcType::RecordAccessor:
        if (reinterpret_cast<RecordAccessor*>(header)->record_type)
            mark_object(&reinterpret_cast<RecordAccessor*>(header)->record_type->header, true);
        break;
    case GcType::RecordMutator:
        if (reinterpret_cast<RecordMutator*>(header)->record_type)
            mark_object(&reinterpret_cast<RecordMutator*>(header)->record_type->header, true);
        break;
    case GcType::Environment:
        gc_trace_environment_children(reinterpret_cast<Environment*>(header), true);
        break;
    case GcType::EnvBox:
        if (reinterpret_cast<EnvBox*>(header)->env)
            mark_environment(reinterpret_cast<EnvBox*>(header)->env, true);
        break;
    }
}

// ── Forward helpers ───────────────────────────────────────────────────────────

static void forward_object_value_fields(GcHeader* header) {
    switch (header->type) {
    case GcType::Cons: {
        auto* c = reinterpret_cast<ConsCell*>(header);
        gc_forward_value(c->car);
        gc_forward_value(c->cdr);
        break;
    }
    case GcType::String: break;
    case GcType::Closure:
        gc_forward_value(reinterpret_cast<SchemeClosure*>(header)->body);
        break;
    case GcType::CaseClosure: {
        auto* cc = reinterpret_cast<CaseClosure*>(header);
        for (auto& clause : cc->clauses)
            gc_forward_value(clause.body);
        break;
    }
    case GcType::Promise:
        gc_forward_value(reinterpret_cast<Promise*>(header)->payload);
        break;
    case GcType::MultiValues: {
        auto* mv = reinterpret_cast<MultiValues*>(header);
        for (auto& v : mv->values) gc_forward_value(v);
        break;
    }
    case GcType::Record: {
        auto* rec = reinterpret_cast<Record*>(header);
        for (auto& fv : rec->field_values) gc_forward_value(fv);
        break;
    }
    case GcType::RecordType: break;
    case GcType::Parameter: {
        auto* p = reinterpret_cast<Parameter*>(header);
        gc_forward_value(p->value);
        gc_forward_value(p->converter);
        break;
    }
    case GcType::ErrorObject: {
        auto* eo = reinterpret_cast<ErrorObject*>(header);
        for (auto& irr : eo->irritants) gc_forward_value(irr);
        break;
    }
    case GcType::Continuation: {
        auto* k = reinterpret_cast<Continuation*>(header);
        gc_forward_continuation_frames(k);
        for (auto& wf : k->wind_snapshot) {
            gc_forward_value(wf.before);
            gc_forward_value(wf.after);
        }
        for (auto& v : k->handler_snapshot) gc_forward_value(v);
        for (auto& v : k->shadow_snapshot)  gc_forward_value(v);
        break;
    }
    case GcType::SyntaxTransformer: {
        auto* st = reinterpret_cast<SyntaxTransformer*>(header);
        for (auto& rule : st->rules) {
            gc_forward_value(rule.pattern);
            gc_forward_value(rule.tmpl);
        }
        break;
    }
    case GcType::Vector: {
        auto* v = reinterpret_cast<SchemeVector*>(header);
        for (auto& elem : v->elements) gc_forward_value(elem);
        break;
    }
    case GcType::Bytevector:
    case GcType::Port:
    case GcType::Complex:
    case GcType::Rational:
    case GcType::Bignum:
    case GcType::Integer:
    case GcType::Real:
    case GcType::Char:
        break;
    case GcType::ExactComplex: {
        auto* ec = reinterpret_cast<ExactComplex*>(header);
        gc_forward_value(ec->re);
        gc_forward_value(ec->im);
        break;
    }
    case GcType::RecordAccessor:
    case GcType::RecordMutator:
        break;  // record_type is never in the nursery
    case GcType::Environment:
        gc_forward_environment_children(reinterpret_cast<Environment*>(header));
        break;
    case GcType::EnvBox:
        gc_copy_forward_env(reinterpret_cast<EnvBox*>(header)->env);
        break;
    }
}

// ── Sweep helpers ─────────────────────────────────────────────────────────────

// DEBUG: when set, free_object leaks instead of deleting.  Used to confirm
// whether a crash is caused by use-after-free of a GC-collected object.
static bool g_gc_leak_instead_of_free = false;
// DEBUG: when set to a specific type, only objects of that type are leaked;
// all others are freed normally.  Set to GcType::Cons (or any) by default;
// only used when g_gc_leak_instead_of_free is true.  -1 = leak all.
static int  g_gc_leak_only_type = -1;
// DEBUG: count of objects leaked per type, for diagnostic output.
static size_t g_gc_leak_counts[32] = {0};

static void free_object(GcHeader* header) {
    if (g_gc_leak_instead_of_free &&
        (g_gc_leak_only_type < 0 || (int)header->type == g_gc_leak_only_type)) {
        if ((int)header->type < 32) ++g_gc_leak_counts[(int)header->type];
        header->next    = nullptr;
        header->forward = nullptr;
        return;
    }
    switch (header->type) {
    case GcType::Cons:              delete reinterpret_cast<ConsCell*>(header);          break;
    case GcType::String:            delete reinterpret_cast<SchemeString*>(header);      break;
    case GcType::Closure:           delete reinterpret_cast<SchemeClosure*>(header);     break;
    case GcType::CaseClosure:       delete reinterpret_cast<CaseClosure*>(header);       break;
    case GcType::Promise:           delete reinterpret_cast<Promise*>(header);           break;
    case GcType::MultiValues:       delete reinterpret_cast<MultiValues*>(header);       break;
    case GcType::Record:            delete reinterpret_cast<Record*>(header);            break;
    case GcType::RecordType:        delete reinterpret_cast<RecordType*>(header);        break;
    case GcType::Parameter:         delete reinterpret_cast<Parameter*>(header);         break;
    case GcType::ErrorObject:       delete reinterpret_cast<ErrorObject*>(header);       break;
    case GcType::Continuation:      delete reinterpret_cast<Continuation*>(header);      break;
    case GcType::SyntaxTransformer: delete reinterpret_cast<SyntaxTransformer*>(header); break;
    case GcType::Vector:            delete reinterpret_cast<SchemeVector*>(header);      break;
    case GcType::Bytevector:        delete reinterpret_cast<SchemeBytevector*>(header);  break;
    case GcType::Port:              delete reinterpret_cast<Port*>(header);              break;
    case GcType::Complex:           delete reinterpret_cast<SchemeComplex*>(header);     break;
    case GcType::ExactComplex:      delete reinterpret_cast<ExactComplex*>(header);      break;
    case GcType::Rational:          delete reinterpret_cast<SchemeRational*>(header);    break;
    case GcType::Bignum: {
        auto* b = reinterpret_cast<SchemeBignum*>(header);
        mpz_clear(&b->value);
        delete b;
        break;
    }
    case GcType::Integer:
        pool_return_integer(reinterpret_cast<SchemeInteger*>(header));
        break;
    case GcType::Real:
        pool_return_real(reinterpret_cast<SchemeReal*>(header));
        break;
    case GcType::Char:
        pool_return_char(reinterpret_cast<SchemeChar*>(header));
        break;
    case GcType::RecordAccessor:    delete reinterpret_cast<RecordAccessor*>(header);    break;
    case GcType::RecordMutator:     delete reinterpret_cast<RecordMutator*>(header);     break;
    case GcType::Environment:
        delete reinterpret_cast<Environment*>(header);
        break;
    case GcType::EnvBox:
        delete reinterpret_cast<EnvBox*>(header);
        break;
    }
}

// ── Nursery evacuation ────────────────────────────────────────────────────────
// Copy each marked nursery ConsCell to a heap-allocated old-gen object.
// Transfer src ownership; delete src on dead cells.

static void nursery_evacuate_and_forward(
    const std::vector<GcHeader*>& freshly_promoted)
{
    if (g_nursery_bump == 0) return;

    std::vector<ConsCell*> evacuated;
    evacuated.reserve(g_nursery_bump);

    for (size_t i = 0; i < g_nursery_bump; ++i) {
        ConsCell* old_cell = &g_nursery[i];
        if (!old_cell->header.marked.load(std::memory_order_relaxed)) {
            // Dead nursery cell: release owned SourceInfo.  alloc_cons clones
            // src on construction, so each cons cell exclusively owns its src.
            delete old_cell->src;
            old_cell->src = nullptr;
            continue;
        }

        auto* new_cell       = new ConsCell{};
        new_cell->car        = old_cell->car;
        new_cell->cdr        = old_cell->cdr;
        new_cell->src        = old_cell->src;   // transfer ownership
        old_cell->src        = nullptr;         // prevent double-delete on reset
        new_cell->header.gen = 1;
        new_cell->header.next = g_old_head;
        g_old_head            = &new_cell->header;
        ++g_old_count;
        old_cell->header.forward = &new_cell->header;

        if (g_gc_phase == GcPhase::Marking)
            shade_gray(&new_cell->header);
        else if (g_gc_phase == GcPhase::Sweeping) {
            new_cell->header.marked.store(true, std::memory_order_relaxed);
            g_promoted_during_sweep.push_back(&new_cell->header);
        }

        evacuated.push_back(new_cell);
    }

    // Forward all live Value slots that might hold stale nursery pointers.

    for (Value* slot : g_value_roots)
        gc_forward_value(*slot);

    for (std::vector<Value>* vec : g_vec_roots)
        if (vec) for (Value& v : *vec) gc_forward_value(v);

    for (auto& hook : g_forward_hooks)
        hook();

    for (GcHeader* obj : g_remembered_set)
        forward_object_value_fields(obj);

    for (GcHeader* obj : freshly_promoted)
        forward_object_value_fields(obj);

    for (ConsCell* cell : evacuated) {
        gc_forward_value(cell->car);
        gc_forward_value(cell->cdr);
    }

    for (GcHeader*& entry : g_gray_stack)
        if (entry && entry->forward) entry = entry->forward;

    g_nursery_bump = 0;
}

// ── Minor GC ─────────────────────────────────────────────────────────────────

static void minor_collect() {
    PROF_SCOPE(gc_minor);
    g_minor_gc_active = true;

    mark_roots(/*minor_only=*/true);

    for (GcHeader* old_obj : g_remembered_set)
        trace_remembered_children(old_obj);

    std::vector<GcHeader*> freshly_promoted;
    {
        GcHeader** prev = &g_young_head;
        unsigned long long _minguard = 0;  // TEMP DEBUG runaway guard
        while (*prev) {
            if (++_minguard > 20000000ull) {
                fprintf(stderr, "[GCGUARD] minor young-walk runaway: young=%zu old=%zu\n",
                        g_young_count, g_old_count);
                fflush(stderr); abort();
            }
            GcHeader* obj = *prev;
            if (obj->marked.load(std::memory_order_relaxed)) {
                obj->marked.store(false, std::memory_order_relaxed);
                obj->gen   = 1;
                *prev      = obj->next;
                obj->next  = g_old_head;
                g_old_head = obj;
                ++g_old_count;
                --g_young_count;
                freshly_promoted.push_back(obj);
                if (g_gc_phase == GcPhase::Sweeping) {
                    obj->marked.store(true, std::memory_order_relaxed);
                    g_promoted_during_sweep.push_back(obj);
                }
            } else {
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

// ── Incremental major GC (single-threaded) ────────────────────────────────────

static void gc_major_start() {
    // Clear stale marked=true on objects promoted during the previous sweep.
    // shade_gray() returns early for already-marked objects, so without this
    // their children would never be traced and would be freed as garbage.
    for (GcHeader* obj : g_promoted_during_sweep)
        obj->marked.store(false, std::memory_order_relaxed);
    g_promoted_during_sweep.clear();

    g_gc_phase = GcPhase::Marking;
    for (Value* slot : g_value_roots) shade_gray_value(*slot);
    for (Environment** slot : g_env_roots)
        if (*slot) shade_gray_env(*slot);
    for (std::vector<Value>* vec : g_vec_roots)
        if (vec) for (Value& v : *vec) shade_gray_value(v);
    for (auto& hook : g_trace_hooks) hook();
}

static void gc_major_step() {
    PROF_SCOPE(gc_major_step);
    // Process up to MARK_STEP_BUDGET gray objects on the main thread.
    size_t processed = 0;
    while (!g_gray_stack.empty() && processed < MARK_STEP_BUDGET) {
        GcHeader* obj = g_gray_stack.back();
        g_gray_stack.pop_back();
        process_gray_object(obj);
        ++processed;
    }
    if (g_gray_stack.empty())
        gc_major_finish();
}

static void gc_major_finish() {
    // Drain any remaining gray objects (should be empty, but be safe).
    while (!g_gray_stack.empty()) {
        GcHeader* obj = g_gray_stack.back();
        g_gray_stack.pop_back();
        process_gray_object(obj);
    }

    g_gc_phase = GcPhase::Sweeping;
    minor_collect();
    g_sweep_cursor = &g_old_head;
}

// ── Incremental sweep step ────────────────────────────────────────────────────

static void gc_sweep_step() {
    size_t processed = 0;
    while (g_sweep_cursor && *g_sweep_cursor && processed < SWEEP_STEP_BUDGET) {
        GcHeader* obj = *g_sweep_cursor;
        if (obj->marked.load(std::memory_order_relaxed)) {
            obj->marked.store(false, std::memory_order_relaxed);
            g_sweep_cursor = &obj->next;
        } else {
            *g_sweep_cursor = obj->next;
            // Erase before free: obj may be in the remembered set (it was old-gen
            // and had a write barrier fired against it).  Remove it here so that
            // minor_collect doesn't trace freed memory via the remembered set.
            g_remembered_set.erase(obj);
            free_object(obj);
            --g_old_count;
        }
        ++processed;
    }
    if (!g_sweep_cursor || !*g_sweep_cursor) {
        g_sweep_cursor = nullptr;
        // Do NOT clear the remembered set here.  Write barriers fired during the
        // sweep phase (old-gen object mutated to point to a new young object) must
        // survive until the next minor_collect consumes them.  Dead objects were
        // already erased from the set above when they were freed.  Any surviving
        // entries are live old-gen objects whose young children need to be traced.

        // Objects promoted during this sweep had marked=true to survive it.
        // Now that the sweep is done, clear those stale marks so the next
        // major GC can shade them correctly.  (gc_major_start also clears them,
        // but if no major GC triggers before check_invariants, we'd fail.)
        for (GcHeader* obj : g_promoted_during_sweep)
            obj->marked.store(false, std::memory_order_relaxed);
        g_promoted_during_sweep.clear();

        g_gc_phase = GcPhase::Idle;
        if (g_old_threshold > 0 && g_old_count >= g_old_threshold)
            g_old_threshold = g_old_count * 2;
    }
}

// ── Public collection entry point ─────────────────────────────────────────────

bool gc_needs_collection() {
    return (g_nursery_bump >= g_nursery_limit)
        || (g_young_threshold > 0 && g_young_count >= g_young_threshold)
        || (g_old_threshold > 0   && g_old_count   >= g_old_threshold)
        || (g_gc_phase == GcPhase::Sweeping);
}

void gc_collect() {
    PROF_SCOPE(gc_collect);

    if (g_gc_phase == GcPhase::Sweeping) {
        if (g_nursery_bump >= g_nursery_limit
                || (g_young_threshold > 0 && g_young_count >= g_young_threshold))
            minor_collect();
        gc_sweep_step();
        return;
    }

    // Phase: Idle.
    // Determine whether we need minor and/or major collection.
    bool need_major = (g_old_threshold > 0 && g_old_count >= g_old_threshold);
    bool need_minor = (g_nursery_bump >= NURSERY_CAPACITY
                   || (g_young_threshold > 0 && g_young_count >= g_young_threshold)
                   || need_major);  // always minor before major

    // Minor collection promotes nursery/young-gen survivors to old-gen and
    // updates all root pointers via forward hooks.  After this, every live
    // object referenced by the CEK state is in old-gen (gen=1), which is the
    // precondition for a correct stop-the-world major collection below.
    if (need_minor)
        minor_collect();

    if (need_major) {
        // Full stop-the-world mark.  shade_gray skips gen!=1 objects, so the
        // minor collection above is required to ensure all live roots are old-gen.
        gc_major_start();
        unsigned long long _majguard = 0;  // TEMP DEBUG runaway guard
        while (!g_gray_stack.empty()) {
            if (++_majguard > 20000000ull) {
                fprintf(stderr, "[GCGUARD] major-mark loop runaway: gray=%zu old=%zu young=%zu\n",
                        g_gray_stack.size(), g_old_count, g_young_count);
                fflush(stderr); abort();
            }
            GcHeader* obj = g_gray_stack.back();
            g_gray_stack.pop_back();
            process_gray_object(obj);
        }
        g_gc_phase = GcPhase::Sweeping;
        g_sweep_cursor = &g_old_head;
    }
}

// ── Public trace wrappers ─────────────────────────────────────────────────────

void gc_trace_value(Value val) {
    if (g_gc_phase == GcPhase::Marking && !g_minor_gc_active)
        shade_gray_value(val);
    else
        mark_value(val, g_minor_gc_active);
}

void gc_trace_environment(Environment* env) {
    if (g_gc_phase == GcPhase::Marking && !g_minor_gc_active)
        shade_gray_env(env);
    else
        mark_environment(env, g_minor_gc_active);
}

// ── Allocation ────────────────────────────────────────────────────────────────

ConsCell* gc_alloc_cons() {
    PROF_COUNT(gc_alloc_cons);
    if (g_nursery_bump < NURSERY_CAPACITY) {
        ConsCell* cell       = &g_nursery[g_nursery_bump++];
        cell->header.marked  = false;
        cell->header.gen     = 0;
        cell->header.type    = GcType::Cons;
        cell->header.next    = nullptr;
        cell->header.forward = nullptr;
        cell->car       = Value{};
        cell->cdr       = Value{};
        cell->src       = nullptr;
        cell->immutable = false;
        return cell;
    }
    auto* cell = new ConsCell{};
    register_young(&cell->header);
    return cell;
}

SchemeString* gc_alloc_string(const std::string& content) {
    auto* s = new SchemeString{content};
    register_young(&s->header);
    return s;
}

SchemeClosure* gc_alloc_closure() {
    auto* cl = new SchemeClosure{};
    register_young(&cl->header);
    return cl;
}

CaseClosure* gc_alloc_case_closure() {
    auto* cc = new CaseClosure{};
    register_young(&cc->header);
    return cc;
}

Promise* gc_alloc_promise(Value payload, bool is_done, bool iterative) {
    auto* p   = new Promise{is_done, std::move(payload), iterative};
    register_young(&p->header);
    return p;
}

MultiValues* gc_alloc_multi_values() {
    auto* mv = new MultiValues{};
    register_young(&mv->header);
    return mv;
}

Record* gc_alloc_record() {
    auto* rec = new Record{};
    register_young(&rec->header);
    return rec;
}

RecordType* gc_alloc_record_type() {
    auto* rt = new RecordType{};
    register_young(&rt->header);
    return rt;
}

Parameter* gc_alloc_parameter(Value val, Value converter) {
    auto* p      = new Parameter{};
    p->value     = std::move(val);
    p->converter = std::move(converter);
    register_young(&p->header);
    return p;
}

ErrorObject* gc_alloc_error_object(std::string msg, std::vector<Value> irr, int kind) {
    auto* e = new ErrorObject{std::move(msg), std::move(irr), kind};
    register_young(&e->header);
    return e;
}

Continuation* gc_alloc_continuation() {
    auto* k = new Continuation{};
    register_young(&k->header);
    return k;
}

SyntaxTransformer* gc_alloc_syntax_transformer() {
    auto* st = new SyntaxTransformer{};
    register_young(&st->header);
    return st;
}

SchemeVector* gc_alloc_vector(size_t n, Value fill) {
    auto* v = new SchemeVector{n, fill};
    register_young(&v->header);
    return v;
}

SchemeBytevector* gc_alloc_bytevector(size_t n, uint8_t fill) {
    auto* bv = new SchemeBytevector{n, fill};
    register_young(&bv->header);
    return bv;
}

Port* gc_alloc_port(bool is_input, bool is_text, const std::string& name) {
    auto* p = new Port{is_input, is_text, name};
    register_young(&p->header);
    return p;
}

SchemeComplex* gc_alloc_complex(double real, double imag) {
    auto* z = new SchemeComplex{real, imag};
    register_young(&z->header);
    return z;
}

ExactComplex* gc_alloc_exact_complex(Value re, Value im) {
    auto* z = new ExactComplex{std::move(re), std::move(im)};
    register_young(&z->header);
    return z;
}

SchemeRational* gc_alloc_rational(int64_t num, int64_t den) {
    auto* r = new SchemeRational{num, den};
    register_young(&r->header);
    return r;
}

RecordAccessor* gc_alloc_record_accessor(RecordType* rt, int idx, std::string name) {
    auto* ra = new RecordAccessor{rt, idx, std::move(name)};
    register_young(&ra->header);
    return ra;
}

RecordMutator* gc_alloc_record_mutator(RecordType* rt, int idx, std::string name) {
    auto* rm = new RecordMutator{rt, idx, std::move(name)};
    register_young(&rm->header);
    return rm;
}

// gc_alloc_environment is implemented in Environment.cpp (requires full struct definition).

// ── Root tracking ─────────────────────────────────────────────────────────────

void gc_root_push(Value* slot) { g_value_roots.push_back(slot); }
void gc_root_pop(Value* slot) {
    for (auto it = g_value_roots.rbegin(); it != g_value_roots.rend(); ++it) {
        if (*it == slot) { g_value_roots.erase(std::next(it).base()); return; }
    }
}

void gc_env_root_push(Environment** slot) { g_env_roots.push_back(slot); }
void gc_env_root_pop(Environment** slot) {
    for (auto it = g_env_roots.rbegin(); it != g_env_roots.rend(); ++it) {
        if (*it == slot) { g_env_roots.erase(std::next(it).base()); return; }
    }
}

void gc_vec_root_push(std::vector<Value>* slot) { g_vec_roots.push_back(slot); }
void gc_vec_root_pop(std::vector<Value>* slot) {
    for (auto it = g_vec_roots.rbegin(); it != g_vec_roots.rend(); ++it) {
        if (*it == slot) { g_vec_roots.erase(std::next(it).base()); return; }
    }
}

void gc_push_trace_hook(GcTraceHook hook)     { g_trace_hooks.push_back(std::move(hook)); }
void gc_pop_trace_hook()
    { if (!g_trace_hooks.empty()) g_trace_hooks.pop_back(); }

void gc_push_forward_hook(GcForwardHook hook) { g_forward_hooks.push_back(std::move(hook)); }
void gc_pop_forward_hook()
    { if (!g_forward_hooks.empty()) g_forward_hooks.pop_back(); }

// ── Threshold / stats ─────────────────────────────────────────────────────────

void gc_set_threshold(size_t t) {
    g_old_threshold   = t;
    g_young_threshold = (t >= 4) ? t / 4 : t;
}

size_t gc_object_count() {
    return g_nursery_bump + g_young_count + g_old_count;
}

// ── Init / shutdown ───────────────────────────────────────────────────────────

void gc_init() {
    g_nursery_bump    = 0;
    g_young_head = g_old_head = nullptr;
    g_young_count = g_old_count = 0;
    // Preserve an active ]gc-stress setting across (re)initialization: the
    // stressed thresholds persist until the listener command toggles them back.
    if (!g_gc_stress) {
        g_young_threshold = 256;
        g_old_threshold   = 1024;
    }
    g_gc_phase        = GcPhase::Idle;
    g_sweep_cursor    = nullptr;
    g_gray_stack.clear();
    g_remembered_set.clear();
    g_value_roots.clear();
    g_env_roots.clear();
    g_vec_roots.clear();
    g_trace_hooks.clear();
    g_forward_hooks.clear();
}

void gc_shutdown() {
    g_value_roots.clear();
    g_env_roots.clear();
    g_vec_roots.clear();
    g_remembered_set.clear();

    auto free_list = [](GcHeader* head) {
        GcHeader* cur = head;
        while (cur) { GcHeader* next = cur->next; free_object(cur); cur = next; }
    };

    // Nursery ConsCells are static — null out src for dead cells, then skip.
    for (size_t i = 0; i < g_nursery_bump; ++i) {
        delete g_nursery[i].src;
        g_nursery[i].src = nullptr;
    }
    g_nursery_bump = 0;

    g_gc_phase     = GcPhase::Idle;
    g_sweep_cursor = nullptr;
    g_gray_stack.clear();

    free_list(g_young_head);
    free_list(g_old_head);
    g_young_head = g_old_head = nullptr;
    g_young_count = g_old_count = 0;
}

// ── GC-stress toggle (see gc.h; exposed via the ]gc-stress listener command) ──

void gc_set_stress(bool on)
   {
   if (on == g_gc_stress) return;
   if (on)
      {
      g_saved_young_threshold = g_young_threshold;
      g_saved_old_threshold   = g_old_threshold;
      g_young_threshold = 64;     // minor GC after a trickle of young objects
      g_old_threshold   = 1024;   // (auto-grows during majors; that's fine)
      g_nursery_limit   = 256;    // evacuate the nursery far more often
      g_gc_stress       = true;
      }
   else
      {
      g_young_threshold = g_saved_young_threshold;
      g_old_threshold   = g_saved_old_threshold;
      g_nursery_limit   = NURSERY_CAPACITY;
      g_gc_stress       = false;
      }
   }

bool gc_stress_enabled() { return g_gc_stress; }

// ── Test-only API (see gc_test_api.h) ─────────────────────────────────────────

#include "gc_test_api.h"

size_t gc_test_young_count()       { return g_young_count; }
size_t gc_test_old_count()         { return g_old_count; }
size_t gc_test_nursery_bump()      { return g_nursery_bump; }
size_t gc_test_nursery_capacity()  { return NURSERY_CAPACITY; }
size_t gc_test_remembered_set_size() { return g_remembered_set.size(); }

int gc_test_gc_phase() {
    switch (g_gc_phase) {
        case GcPhase::Idle:     return 0;
        case GcPhase::Marking:  return 1;
        case GcPhase::Sweeping: return 2;
    }
    return -1;
}

size_t gc_test_young_threshold()      { return g_young_threshold; }
size_t gc_test_old_threshold()        { return g_old_threshold; }
void   gc_test_set_young_threshold(size_t v) { g_young_threshold = v; }
void   gc_test_set_old_threshold(size_t v)   { g_old_threshold   = v; }

size_t gc_test_walk_young(std::function<void(GcHeader*)> fn) {
    size_t n = 0;
    for (GcHeader* h = g_young_head; h; h = h->next) { fn(h); ++n; }
    return n;
}

size_t gc_test_walk_old(std::function<void(GcHeader*)> fn) {
    size_t n = 0;
    for (GcHeader* h = g_old_head; h; h = h->next) { fn(h); ++n; }
    return n;
}

size_t gc_test_walk_nursery(std::function<void(GcHeader*)> fn) {
    for (size_t i = 0; i < g_nursery_bump; ++i)
        fn(&g_nursery[i].header);
    return g_nursery_bump;
}

bool gc_test_in_heap(GcHeader* header) {
    if (!header) return false;
    // Nursery: pointer range check.
    auto* base = reinterpret_cast<GcHeader*>(&g_nursery[0]);
    auto* end  = reinterpret_cast<GcHeader*>(&g_nursery[g_nursery_bump]);
    if (header >= base && header < end) return true;
    for (GcHeader* h = g_young_head; h; h = h->next)
        if (h == header) return true;
    for (GcHeader* h = g_old_head; h; h = h->next)
        if (h == header) return true;
    return false;
}

bool gc_test_in_remembered_set(GcHeader* header) {
    return g_remembered_set.count(header) > 0;
}

void gc_test_reset() {
    gc_shutdown();
    // Restore defaults that gc_shutdown leaves alone.
    g_gc_stress       = false;   // never let GC-stress leak between tests
    g_nursery_limit   = NURSERY_CAPACITY;
    g_young_threshold = 256;
    g_old_threshold   = 1024;
    // Tests run sequentially in one binary; ensure no lingering hooks from a
    // previous test reference stack frames that are now gone.
    g_trace_hooks.clear();
    g_forward_hooks.clear();
    g_promoted_during_sweep.clear();
}

void gc_test_force_minor() {
    minor_collect();
}

size_t gc_test_trace_hook_count() { return g_trace_hooks.size(); }
void   gc_test_set_leak_instead_of_free(bool on) {
    g_gc_leak_instead_of_free = on;
    if (on) for (auto& c : g_gc_leak_counts) c = 0;
}
void   gc_test_set_leak_only_type(int t) { g_gc_leak_only_type = t; }
size_t gc_test_leak_count(int t) {
    if (t < 0 || t >= 32) return 0;
    return g_gc_leak_counts[t];
}

void gc_test_force_major() {
    // Sweep first if a previous cycle left us in Sweeping.
    while (g_gc_phase == GcPhase::Sweeping) gc_sweep_step();
    // Promote all live young/nursery to old-gen so major-GC roots are gen=1.
    minor_collect();
    // Full stop-the-world mark.
    gc_major_start();
    while (!g_gray_stack.empty()) {
        GcHeader* obj = g_gray_stack.back();
        g_gray_stack.pop_back();
        process_gray_object(obj);
    }
    g_gc_phase = GcPhase::Sweeping;
    g_sweep_cursor = &g_old_head;
    // Drive sweep to completion.
    while (g_gc_phase == GcPhase::Sweeping) gc_sweep_step();
}
