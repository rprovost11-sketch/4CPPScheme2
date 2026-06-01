#pragma once
// gc.h -- generational GC interface.
// Adapted from CPPScheme's gc.h/gc_gen.cpp for CPPScheme2's type set.
// GcType and GcHeader are defined in AST.h (tight coupling to Value layout).

#include "AST.h"
#include "scheme_export.h"
#include <cstddef>
#include <functional>

// ── Allocation ────────────────────────────────────────────────────────────────
// All heap-allocated Scheme objects are created through these functions.
// Allocators deliberately do NOT trigger collection -- see gc_needs_collection().

CPPSCHEME2_API ConsCell*          gc_alloc_cons();
CPPSCHEME2_API SchemeString*      gc_alloc_string(const std::string& content);
CPPSCHEME2_API SchemeClosure*     gc_alloc_closure();
CPPSCHEME2_API CaseClosure*       gc_alloc_case_closure();
CPPSCHEME2_API Promise*           gc_alloc_promise(Value payload, bool is_done, bool iterative = false);
CPPSCHEME2_API MultiValues*       gc_alloc_multi_values();
CPPSCHEME2_API Record*            gc_alloc_record();
CPPSCHEME2_API RecordType*        gc_alloc_record_type();
CPPSCHEME2_API Parameter*         gc_alloc_parameter(Value val, Value converter);
CPPSCHEME2_API ErrorObject*       gc_alloc_error_object(std::string msg,
                                                        std::vector<Value> irr,
                                                        int kind);
CPPSCHEME2_API Continuation*      gc_alloc_continuation();
CPPSCHEME2_API SyntaxTransformer* gc_alloc_syntax_transformer();
CPPSCHEME2_API SchemeVector*      gc_alloc_vector(size_t n, Value fill = Value{});
CPPSCHEME2_API SchemeBytevector*  gc_alloc_bytevector(size_t n, uint8_t fill = 0);
CPPSCHEME2_API Port*              gc_alloc_port(bool is_input, bool is_text,
                                               const std::string& name);
CPPSCHEME2_API SchemeComplex*     gc_alloc_complex(double real, double imag);
CPPSCHEME2_API ExactComplex*      gc_alloc_exact_complex(Value re, Value im);
CPPSCHEME2_API SchemeRational*    gc_alloc_rational(int64_t num, int64_t den);
CPPSCHEME2_API RecordAccessor*    gc_alloc_record_accessor(RecordType* rt, int idx,
                                                           std::string name);
CPPSCHEME2_API RecordMutator*     gc_alloc_record_mutator(RecordType* rt, int idx,
                                                          std::string name);
CPPSCHEME2_API Environment*       gc_alloc_environment(Environment* parent);
CPPSCHEME2_API EnvBox*            gc_alloc_env_box(Environment* env);

// ── Root tracking ─────────────────────────────────────────────────────────────
// Register Value* and Environment** slots that are live on the C++ stack.
// Use GcRootGuard (below) rather than calling these directly.

CPPSCHEME2_API void gc_root_push(Value* slot);
CPPSCHEME2_API void gc_root_pop(Value* slot);

// RAII root guard -- keeps a Value visible to the GC for its lifetime.
struct GcRootGuard {
    Value val;

    explicit GcRootGuard(Value initial = Value{}) : val(initial)
        { gc_root_push(&val); }
    ~GcRootGuard()
        { gc_root_pop(&val); }

    GcRootGuard(const GcRootGuard&)            = delete;
    GcRootGuard& operator=(const GcRootGuard&) = delete;

    operator Value() const { return val; }
    Value& get()           { return val; }
};

CPPSCHEME2_API void gc_env_root_push(Environment** slot);
CPPSCHEME2_API void gc_env_root_pop(Environment** slot);

// Register a std::vector<Value> as a root.  All elements are traced (and
// forwarded) for as long as the registration is active.  Vector mutation
// (push_back, move-assign, etc.) is safe -- the GC re-reads vec->data() at
// each collection.
CPPSCHEME2_API void gc_vec_root_push(std::vector<Value>* slot);
CPPSCHEME2_API void gc_vec_root_pop(std::vector<Value>* slot);

// RAII guard for a std::vector<Value> root.
struct GcRootVec {
    std::vector<Value>* vec;
    explicit GcRootVec(std::vector<Value>& v) : vec(&v) { gc_vec_root_push(vec); }
    ~GcRootVec() { gc_vec_root_pop(vec); }
    GcRootVec(const GcRootVec&)            = delete;
    GcRootVec& operator=(const GcRootVec&) = delete;
};

// ── Public trace functions ────────────────────────────────────────────────────
// Called from continuation frame trace implementations.

CPPSCHEME2_API void gc_trace_value(Value val);
CPPSCHEME2_API void gc_trace_environment(Environment* env);

// ── Hook registration ─────────────────────────────────────────────────────────
// The CEK evaluator registers trace/forward hooks while cek_eval() is active.

using GcTraceHook   = std::function<void()>;
using GcForwardHook = std::function<void()>;

CPPSCHEME2_API void gc_push_trace_hook(GcTraceHook hook);
CPPSCHEME2_API void gc_pop_trace_hook();
CPPSCHEME2_API void gc_push_forward_hook(GcForwardHook hook);
CPPSCHEME2_API void gc_pop_forward_hook();

// ── Moving-GC pointer update ──────────────────────────────────────────────────
CPPSCHEME2_API void gc_copy_forward_value(Value& val);
CPPSCHEME2_API void gc_copy_forward_env(Environment*& env_ptr);

// ── Write barrier ─────────────────────────────────────────────────────────────
CPPSCHEME2_API void gc_write_barrier(GcHeader* parent_obj, GcHeader* child_obj);

// ── Continuation frame hooks (implemented in Evaluator.cpp) ───────────────────
CPPSCHEME2_API void gc_trace_continuation_frames(Continuation* cont);
CPPSCHEME2_API void gc_forward_continuation_frames(Continuation* cont);

// ── Environment trace hooks (implemented in Environment.cpp) ──────────────────
CPPSCHEME2_API void gc_trace_environment_children(Environment* env, bool minor_only);
CPPSCHEME2_API void gc_forward_environment_children(Environment* env);

// ── Collection ────────────────────────────────────────────────────────────────
CPPSCHEME2_API bool   gc_needs_collection();
CPPSCHEME2_API void   gc_collect();
CPPSCHEME2_API void   gc_set_threshold(size_t threshold);
CPPSCHEME2_API size_t gc_object_count();

// ── Init / shutdown ───────────────────────────────────────────────────────────
CPPSCHEME2_API void gc_init();
CPPSCHEME2_API void gc_shutdown();

// ── Internal registration helper ─────────────────────────────────────────────
// Called by gc_alloc_environment (implemented in Environment.cpp, where the
// full Environment struct is visible) to register the new object with the GC.
CPPSCHEME2_API void gc_register_young(GcHeader* header);
