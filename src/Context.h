#pragma once
// Context.h -- interpreter context.
// Direct port of pyscheme/Context.py.
#include "AST.h"
#include "scheme_export.h"
#include <chrono>
#include <functional>
#include <iosfwd>
#include <vector>

struct Tracer;
class Debugger;

using SteadyClock = std::chrono::steady_clock;
using SteadyTimePoint = std::chrono::time_point<SteadyClock>;

struct CPPSCHEME2_API Context
   {
   std::ostream* outStrm;
   bool _debugging = false;
   bool _instrumented = false;
   Debugger* debugger = nullptr;
   Tracer* tracer = nullptr;
   std::function<Value(Environment*, const Value&)> lEval;
   std::vector<WindFrame> wind_stack;
   std::vector<Value> handler_stack;
   std::vector<ShadowEntry> shadow_stack;
   SteadyTimePoint timeout_at; // deadline; zero-init = disabled
   bool timeout_active = false;
   uint32_t _timeout_step = 0;

   // Continuation-escape bookkeeping.  Each cek_loop invocation claims a unique
   // id from eval_id_counter and publishes it in current_eval_id while running;
   // continuations record the id of the loop that captured them.  eval_id_stack
   // holds the ids of every loop currently live on the C++ stack (outermost
   // first), so an invocation can tell whether a continuation's owning loop is
   // still an ancestor (escape -- unwind to it) or has already returned
   // (re-entry -- install in place, as the original machine always did).
   // See Evaluator.cpp ContinuationEscape and eval_id_active.
   uint64_t eval_id_counter = 0;
   uint64_t current_eval_id = 0;
   std::vector<uint64_t> eval_id_stack;

   // nullptr → defaults to std::cout
   explicit Context(std::ostream* out = nullptr);

   void _update_instrumented();
   void write(const std::string& text);
   };
