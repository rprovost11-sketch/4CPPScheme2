#pragma once
// gc_test_api.h -- test-only accessors into the generational GC internals.
//
// Intended for the gc_test binary.  Do NOT use from application code; these
// expose state that's normally encapsulated within gc_gen.cpp.

#include "AST.h"
#include "scheme_export.h"
#include <cstddef>
#include <functional>

// ── Counts and phase ──────────────────────────────────────────────────────────

CEKSCHEME_API size_t gc_test_young_count();
CEKSCHEME_API size_t gc_test_old_count();
CEKSCHEME_API size_t gc_test_nursery_bump();
CEKSCHEME_API size_t gc_test_nursery_capacity();
CEKSCHEME_API size_t gc_test_remembered_set_size();

// 0 = Idle, 1 = Marking, 2 = Sweeping.
CEKSCHEME_API int    gc_test_gc_phase();

CEKSCHEME_API size_t gc_test_young_threshold();
CEKSCHEME_API size_t gc_test_old_threshold();
CEKSCHEME_API void   gc_test_set_young_threshold(size_t v);
CEKSCHEME_API void   gc_test_set_old_threshold(size_t v);

// ── Heap walking ──────────────────────────────────────────────────────────────
// Invoke fn for every header in the young (g_young_head) or old (g_old_head)
// linked list.  Nursery cells are walked separately because they're in a fixed
// array and don't belong to either list.

CEKSCHEME_API size_t gc_test_walk_young(std::function<void(GcHeader*)> fn);
CEKSCHEME_API size_t gc_test_walk_old(std::function<void(GcHeader*)> fn);
CEKSCHEME_API size_t gc_test_walk_nursery(std::function<void(GcHeader*)> fn);

// True iff header is reachable via g_young_head list, g_old_head list, or the
// active nursery prefix.  Used to verify "object survived this GC cycle."
CEKSCHEME_API bool   gc_test_in_heap(GcHeader* header);
CEKSCHEME_API bool   gc_test_in_remembered_set(GcHeader* header);

// Reset all GC state to a freshly-initialized condition.  Frees all live
// objects.  Tests call this between cases for isolation.
CEKSCHEME_API void   gc_test_reset();

// Unconditionally run a minor collection (mark young from roots, promote
// survivors, evacuate nursery, forward pointers).  Bypasses the threshold
// checks in gc_collect() so tests can control timing precisely.
CEKSCHEME_API void   gc_test_force_minor();

// Unconditionally run a complete major collection: forced minor first, then
// full stop-the-world mark, then synchronous sweep to completion.  Phase
// returns to Idle on success.
CEKSCHEME_API void   gc_test_force_major();

CEKSCHEME_API size_t gc_test_trace_hook_count();

// DEBUG: turn off free_object.  Used to confirm whether a crash is caused by
// use-after-free of an object the GC collected (i.e. a missing root).
CEKSCHEME_API void   gc_test_set_leak_instead_of_free(bool on);
// DEBUG: when leak mode is on, leak only objects of this GcType.  Set to -1
// to leak all types.
CEKSCHEME_API void   gc_test_set_leak_only_type(int gc_type_int);

// DEBUG: returns the count of objects that would have been freed by type
// (indexed by GcType-as-int).  Only populated while leak mode is on.
CEKSCHEME_API size_t gc_test_leak_count(int gc_type_int);
