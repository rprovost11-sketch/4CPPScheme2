#pragma once
#include "value.h"
#include "scheme_export.h"
#include <cstddef>
#include <functional>

// ── GC allocation ─────────────────────────────────────────────────────────────
SCHEME_API ConsCell*             gc_alloc_cons();
SCHEME_API SchemeString*         gc_alloc_string(const std::string& content);
SCHEME_API SchemeClosure*        gc_alloc_closure();
SCHEME_API SchemeMacro*          gc_alloc_macro();
SCHEME_API SchemeSyntaxTransformer* gc_alloc_syntax_transformer();
SCHEME_API SchemeContinuation*   gc_alloc_continuation();
SCHEME_API SchemeVector*         gc_alloc_vector(size_t n, Value fill = Value{});
SCHEME_API SchemeBytevector*     gc_alloc_bytevector(size_t n, uint8_t fill = 0);
SCHEME_API Environment*          gc_alloc_environment(Environment* parent);
SCHEME_API SchemePromise*        gc_alloc_promise(Value thunk);
SCHEME_API SchemeParameter*      gc_alloc_parameter(Value init, Value converter);
SCHEME_API SchemePort*           gc_alloc_port();
SCHEME_API SchemeComplex*        gc_alloc_complex(double real_part, double imag_part);
SCHEME_API SchemeRational*       gc_alloc_rational(int64_t num, int64_t den);
SCHEME_API SchemeCaseClosure*    gc_alloc_case_closure();
SCHEME_API SchemeMultiValues*    gc_alloc_multi_values();
SCHEME_API SchemeRecord*         gc_alloc_record();
SCHEME_API SchemeExactComplex*   gc_alloc_exact_complex(Value real_part, Value imag_part);
SCHEME_API SchemeErrorObject*    gc_alloc_error_object();

// ── Root tracking ─────────────────────────────────────────────────────────────
SCHEME_API void gc_root_push(Value* slot);
SCHEME_API void gc_root_pop(Value* slot);

struct GcRootGuard
   {
   Value val;

   explicit GcRootGuard(Value initial = Value{}) : val(initial) { gc_root_push(&val); }
   ~GcRootGuard() { gc_root_pop(&val); }

   GcRootGuard(const GcRootGuard&)            = delete;
   GcRootGuard& operator=(const GcRootGuard&) = delete;

   operator Value() const { return val; }
   Value& get()           { return val; }
   };

SCHEME_API void gc_env_root_push(Environment** slot);
SCHEME_API void gc_env_root_pop(Environment** slot);

// ── Public trace functions ────────────────────────────────────────────────────
SCHEME_API void gc_trace_value(Value val);
SCHEME_API void gc_trace_environment(Environment* env);

// ── K-stack trace hooks ───────────────────────────────────────────────────────
using GcTraceHook   = std::function<void()>;
using GcForwardHook = std::function<void()>;

SCHEME_API void gc_push_trace_hook(GcTraceHook hook);
SCHEME_API void gc_pop_trace_hook();
SCHEME_API void gc_push_forward_hook(GcForwardHook hook);
SCHEME_API void gc_pop_forward_hook();

// ── Moving-GC pointer update ──────────────────────────────────────────────────
SCHEME_API void gc_copy_forward_value(Value& val);
SCHEME_API void gc_copy_forward_env(Environment*& env_ptr);

// ── Write barrier ─────────────────────────────────────────────────────────────
SCHEME_API void gc_write_barrier(GcHeader* parent_obj, GcHeader* child_obj);

// ── Continuation frame hooks (implemented in evaluator.cpp) ───────────────────
SCHEME_API void gc_trace_continuation_frames(SchemeContinuation* cont);
SCHEME_API void gc_forward_continuation_frames(SchemeContinuation* cont);

// ── Collection ────────────────────────────────────────────────────────────────
SCHEME_API bool   gc_needs_collection();
SCHEME_API void   gc_collect();
SCHEME_API void   gc_set_threshold(size_t threshold);
SCHEME_API size_t gc_object_count();

// ── Init / shutdown ───────────────────────────────────────────────────────────
SCHEME_API void gc_init();
SCHEME_API void gc_shutdown();
