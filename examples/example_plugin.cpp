// example_plugin.cpp -- a minimal cppScheme2 native plugin.
//
// Builds to a shared library (example_plugin.dll) linking cppscheme2_core.
// When colocated with a library's .sld as <lib>.dll and `(import (lib))` is
// resolved, the loader calls cppscheme2_plugin_init, which installs the
// primitives below into the global environment.  See scheme_plugin_api.h.
//
// Demonstrates the ABI: a plugin only needs scheme_plugin_api.h + the exported
// core API (make_integer / make_string / ...) to build native primitives.
#include "scheme_plugin_api.h"

namespace {

// BuiltinFn = Value(Context*, Environment*, std::vector<Value>&, const Value*)
Value native_answer(Context*, Environment*, std::vector<Value>&, const Value*)
   {
   return make_integer(42);
   }

Value native_hello(Context*, Environment*, std::vector<Value>&, const Value*)
   {
   return make_string("hello-from-native-dll");
   }

}  // namespace

extern "C" CPPSCHEME2_PLUGIN_EXPORT void cppscheme2_plugin_init(Environment* env)
   {
   scheme_register_primitive(env, "native-answer", native_answer);
   scheme_register_primitive(env, "native-hello", native_hello);
   }
