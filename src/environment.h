#pragma once
#include "value.h"
#include "scheme_export.h"
#include <unordered_map>
#include <optional>
#include <string_view>

// ── Helpers ───────────────────────────────────────────────────────────────────

// Strip hygiene gensym prefix \x01h.BASE.N from a symbol name for error messages.
SCHEME_API std::string display_name_of(uint32_t sid);

// Build "f: N argument(s) provided; M expected." style text.
// hi < 0 means variadic (at least lo args).
SCHEME_API std::string arity_mismatch_msg(std::string_view name, int lo, int hi, int n_provided);

// ── Environment ───────────────────────────────────────────────────────────────
// Lexical environment frame.  GC-managed so closures can hold Environment*
// across collections.  Bindings map symbol IDs (uint32_t) to Values.
//
// parent chain  — lookup and set! walk up through parent until root (nullptr).
// global cache  — every frame caches a pointer to the chain root.
// freeze        — marks the frame immutable; bind/set on a frozen frame raises
//                 SchemeTypeError.

struct Environment
   {
   GcHeader     header{GcType::Environment};
   Environment* parent = nullptr;
   Environment* global = nullptr;   // cached root of the parent chain
   bool         is_frozen = false;
   std::unordered_map<uint32_t, Value> bindings_;

   explicit Environment(Environment* parent_env = nullptr);
   ~Environment() = default;

   Environment(const Environment&)            = delete;
   Environment& operator=(const Environment&) = delete;

   // Look up sid walking the parent chain. Raises SchemeUnboundError if not found.
   SCHEME_API Value lookup_id(uint32_t sid) const;

   // Look up sid walking the parent chain. Returns empty optional if not found.
   SCHEME_API std::optional<Value> lookup_optional_id(uint32_t sid) const;

   // Bind (or rebind) sid in THIS frame. Raises SchemeTypeError if frozen.
   SCHEME_API void bind_id(uint32_t sid, Value val);

   // Update the nearest binding of sid in the chain.
   // Raises SchemeUnboundError if not found; SchemeTypeError if owning frame is frozen.
   SCHEME_API void set_id(uint32_t sid, Value val);

   // Mark this frame immutable. Idempotent.
   SCHEME_API void freeze();

   // Return the chain root (global environment).
   Environment* get_global() const { return global; }

   // String-keyed wrappers (intern on each call; use _id variants on hot paths).
   SCHEME_API Value                lookup(std::string_view name) const;
   SCHEME_API std::optional<Value> lookup_optional(std::string_view name) const;
   SCHEME_API void                 bind(std::string_view name, Value val);
   SCHEME_API void                 set(std::string_view name, Value val);
   };
