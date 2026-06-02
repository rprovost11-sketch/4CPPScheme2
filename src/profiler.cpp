// profiler.cpp -- CEK machine profiling counters.
// Straight copy from CPPScheme; no PyScheme counterpart.
// Compile with -DPROFILE_COUNTERS to enable.
#include "profiler.h"

#ifdef PROFILE_COUNTERS

#include <iostream>
#include <iomanip>
#include <cstring>
#include <chrono>
#include <thread>

ProfData g_prof;

// ── TSC calibration ─────────────────────────────────────────────────────────
// Measure TSC ticks over a short sleep calibrated by steady_clock.

void prof_init()
   {
   using clock = std::chrono::steady_clock;
   auto t0 = clock::now();
   auto tsc0 = __rdtsc();
   std::this_thread::sleep_for(std::chrono::milliseconds(50));
   auto tsc1 = __rdtsc();
   auto t1 = clock::now();
   double elapsed = std::chrono::duration<double>(t1 - t0).count();
   g_prof.tsc_freq = static_cast<double>(tsc1 - tsc0) / elapsed;
   }

// ── Formatting helpers ──────────────────────────────────────────────────────

static void print_row(const char* label, const ProfEntry& e, double tsc_freq)
   {
   if (e.count == 0)
      return;
   double secs = (tsc_freq > 0.0) ? static_cast<double>(e.total_tsc) / tsc_freq : 0.0;
   double avg_us = (e.count > 0 && tsc_freq > 0.0)
                       ? (secs / static_cast<double>(e.count)) * 1e6
                       : 0.0;
   std::cout << "  " << std::left << std::setw(22) << label
             << std::right << std::setw(14) << e.count
             << std::setw(12) << std::fixed << std::setprecision(3) << secs
             << std::setw(12) << std::fixed << std::setprecision(3) << avg_us
             << "\n";
   }

void prof_report()
   {
   std::cout << "\n  ── Profile Report ──────────────────────────────────────────\n";
   std::cout << "  " << std::left << std::setw(22) << "Function"
             << std::right << std::setw(14) << "Calls"
             << std::setw(12) << "Total(s)"
             << std::setw(12) << "Avg(us)"
             << "\n";
   std::cout << "  " << std::string(60, '-') << "\n";

   std::cout << "  [CEK loop]\n";
   print_row("eval step", g_prof.cek_eval_step, g_prof.tsc_freq);
   print_row("apply step", g_prof.cek_apply_step, g_prof.tsc_freq);

   std::cout << "  [Procedure dispatch]\n";
   print_row("apply builtin", g_prof.apply_builtin, g_prof.tsc_freq);
   print_row("apply lambda", g_prof.apply_lambda, g_prof.tsc_freq);
   print_row("apply continuation", g_prof.apply_continuation, g_prof.tsc_freq);

   std::cout << "  [Frame apply]\n";
   print_row("IfFrame", g_prof.frame_if, g_prof.tsc_freq);
   print_row("BeginFrame", g_prof.frame_begin, g_prof.tsc_freq);
   print_row("EvalOpFrame", g_prof.frame_eval_op, g_prof.tsc_freq);
   print_row("EvalArgsFrame", g_prof.frame_eval_args, g_prof.tsc_freq);
   print_row("LetFrame", g_prof.frame_let, g_prof.tsc_freq);
   print_row("LetrecStarFrame", g_prof.frame_letrec_star, g_prof.tsc_freq);
   print_row("AndFrame", g_prof.frame_and, g_prof.tsc_freq);
   print_row("OrFrame", g_prof.frame_or, g_prof.tsc_freq);
   print_row("DefineFrame", g_prof.frame_define, g_prof.tsc_freq);
   print_row("SetFrame", g_prof.frame_set, g_prof.tsc_freq);

   std::cout << "  [Environment]\n";
   print_row("lookup", g_prof.env_lookup, g_prof.tsc_freq);
   print_row("define", g_prof.env_define, g_prof.tsc_freq);
   print_row("set", g_prof.env_set, g_prof.tsc_freq);

   std::cout << "  [GC]\n";
   print_row("gc_collect", g_prof.gc_collect, g_prof.tsc_freq);
   print_row("minor collect", g_prof.gc_minor, g_prof.tsc_freq);
   print_row("major step", g_prof.gc_major_step, g_prof.tsc_freq);
   print_row("alloc cons", g_prof.gc_alloc_cons, g_prof.tsc_freq);

   std::cout << "  [Other]\n";
   print_row("eval_compound", g_prof.eval_compound, g_prof.tsc_freq);

   std::cout << "  " << std::string(60, '-') << "\n\n";
   }

void prof_reset()
   {
   double freq = g_prof.tsc_freq; // preserve calibration
   g_prof = ProfData{};
   g_prof.tsc_freq = freq;
   }

#endif // PROFILE_COUNTERS
