#pragma once
// primitives/debug.h -- debugging primitives.
// Direct port of pyscheme/primitives/debug.py.
#include "../AST.h"
#include "../scheme_export.h"

struct Context;

// Non-interactive inspect: print structured description of val.
// Exposed for use by the Debugger (safe_inspect command).
CPPSCHEME2_API void debug_run_inspect(const Value& val, Context* ctx);

CPPSCHEME2_API void register_debug();
