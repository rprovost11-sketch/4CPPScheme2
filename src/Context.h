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

using SteadyClock   = std::chrono::steady_clock;
using SteadyTimePoint = std::chrono::time_point<SteadyClock>;

struct CEKSCHEME_API Context {
    std::ostream*            outStrm;
    bool                     _debugging    = false;
    bool                     _instrumented = false;
    Debugger*                debugger      = nullptr;
    Tracer*                  tracer        = nullptr;
    std::function<Value(Environment*, const Value&)> lEval;
    std::vector<WindFrame>   wind_stack;
    std::vector<Value>       handler_stack;
    std::vector<ShadowEntry> shadow_stack;
    SteadyTimePoint          timeout_at;      // deadline; zero-init = disabled
    bool                     timeout_active = false;
    uint32_t                 _timeout_step  = 0;

    // nullptr → defaults to std::cout
    explicit Context(std::ostream* out = nullptr);

    void _update_instrumented();
    void write(const std::string& text);
};
