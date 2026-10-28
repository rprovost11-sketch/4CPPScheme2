#include "scheme_plugin_api.h"
// plugin_api_impl.cpp -- core-side helpers exposed to native plugins.

// Bind `name` -> a primitive wrapping `fn` in `env`.  Runs inside the core, so
// Environment::bind, global_la_key and make_primitive are directly available.
void scheme_register_primitive(Environment* env, const std::string& name,
                               BuiltinFn fn)
   {
   env->bind(global_la_key(intern_symbol(name)), make_primitive(name, std::move(fn)));
   }
