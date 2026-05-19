#pragma once
#include "value.h"
#include "environment.h"
#include "scheme_export.h"
#include "exceptions.h"
#include <iosfwd>
#include <vector>
#include <string>

// Exception carrying a first-class Scheme value from (raise ...).
class SCHEME_API SchemeRaisedException : public SchemeError
   {
public:
   Value raised;
   bool  continuable;
   SchemeRaisedException(Value v, bool cont)
      : SchemeError("raised"), raised(v), continuable(cont) {}
   };

// Per-evaluation interpreter context (shared across nested cek_eval calls).
struct CekCtx
   {
   std::ostream*          out = nullptr;   // nullptr -> stdout
   std::vector<WindFrame> wind_stack;
   std::vector<Value>     handler_stack;
   struct ShadowEntry { std::string label; int count = 1; };
   std::vector<ShadowEntry> shadow_stack;
   };

// Main entry point.  ctx == nullptr creates a thread-local default context.
SCHEME_API Value cek_eval(Value expr, Environment* env, CekCtx* ctx = nullptr);

// Run proc(args...) via the CEK machine.  Used by dynamic-wind thunks,
// force, and parameterize.  env is only used as a fallback if proc is a
// builtin that needs it (rare); closures carry their own env.
SCHEME_API Value apply_scheme_proc(Value proc, const std::vector<Value>& args,
                                    CekCtx& ctx, Environment* env = nullptr);
