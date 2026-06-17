#include "scheme_plugin_api.h"
// plugin_api_impl.cpp -- core-side helpers exposed to native plugins.

// Bind `name` -> a primitive wrapping `fn` in `env`.  Runs inside the core, so
// Environment::bind and make_primitive are directly available.
void scheme_register_primitive(Environment* env, const std::string& name,
                               BuiltinFn fn)
   {
   env->bind(name, make_primitive(name, std::move(fn)));
   }
