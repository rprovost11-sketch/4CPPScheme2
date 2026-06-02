#pragma once
// ── CEK machine profiling counters ──────────────────────────────────────────
// Compile with -DPROFILE_COUNTERS to enable.  When disabled, all macros
// expand to nothing — zero cost.
//
// Usage: place PROF_SCOPE(name) at the top of a function to count calls
// and accumulate inclusive wall time (via __rdtsc).  Call prof_report()
// from the ]profile REPL command.
// Straight copy from CPPScheme; no PyScheme counterpart.

#ifdef PROFILE_COUNTERS

#include "scheme_export.h"
#include <cstdint>
#include <intrin.h> // __rdtsc, __cpuid

// ── Per-function entry ──────────────────────────────────────────────────────

struct ProfEntry
   {
   uint64_t count = 0;
   uint64_t total_tsc = 0;
   };

// ── Aggregate profile data ──────────────────────────────────────────────────

struct ProfData
   {
   // CEK loop
   ProfEntry cek_eval_step;  // one Eval iteration
   ProfEntry cek_apply_step; // one Apply iteration (frame pop)

   // apply_procedure dispatch
   ProfEntry apply_builtin;
   ProfEntry apply_lambda;
   ProfEntry apply_continuation;

   // Frame apply methods
   ProfEntry frame_if;
   ProfEntry frame_begin;
   ProfEntry frame_eval_op;
   ProfEntry frame_eval_args;
   ProfEntry frame_let;
   ProfEntry frame_letrec_star;
   ProfEntry frame_and;
   ProfEntry frame_or;
   ProfEntry frame_define;
   ProfEntry frame_set;

   // Environment
   ProfEntry env_lookup;
   ProfEntry env_define;
   ProfEntry env_set;

   // GC
   ProfEntry gc_collect;
   ProfEntry gc_minor;
   ProfEntry gc_major_step;
   ProfEntry gc_alloc_cons;

   // eval_compound (special form dispatch)
   ProfEntry eval_compound;

   double tsc_freq = 0.0; // ticks per second, calibrated at startup
   };

CPPSCHEME2_API extern ProfData g_prof;

// ── RAII scope timer ────────────────────────────────────────────────────────

struct ProfScope
   {
   ProfEntry& entry;
   uint64_t start;

   ProfScope(ProfEntry& e) : entry(e), start(__rdtsc())
      {
      ++entry.count;
      }
   ~ProfScope()
      {
      entry.total_tsc += __rdtsc() - start;
      }
   };

   // ── Macros ──────────────────────────────────────────────────────────────────
   // PROF_SCOPE(name) — starts timing; accumulates on scope exit.
   // PROF_COUNT(name) — count-only bump, no timing.

#define PROF_SCOPE(name) ProfScope _prof_scope_##name(g_prof.name)
#define PROF_COUNT(name) (++g_prof.name.count)

// ── Public interface ────────────────────────────────────────────────────────

// Calibrate tsc_freq.  Call once at startup (e.g. from eval_init).
CPPSCHEME2_API void prof_init();

// Print a formatted report to stdout, then reset all counters.
CPPSCHEME2_API void prof_report();

// Reset all counters without printing.
CPPSCHEME2_API void prof_reset();

#else // PROFILE_COUNTERS not defined

// All macros expand to nothing.
#define PROF_SCOPE(name) ((void)0)
#define PROF_COUNT(name) ((void)0)

inline void prof_init() {}
inline void prof_report() {}
inline void prof_reset() {}

#endif // PROFILE_COUNTERS
