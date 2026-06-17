#pragma once
// scheme_plugin_api.h -- native-plugin ABI for cppScheme2.
//
// A plugin is a shared library (.dll / .so) colocated with a library's .sld,
// e.g. srfi/<n>.dll next to srfi/<n>.sld.  When `(import (srfi <n>))` is
// resolved, the loader loads the plugin BEFORE the .sld and calls its init
// entry, which installs native primitives the portable .sld can then build on.
// This mirrors pyScheme's `.py` register(env) mechanism (loaded .py-then-.sld).
//
// A plugin must export, with C linkage and the name CPPSCHEME2_PLUGIN_INIT:
//
//     extern "C" CPPSCHEME2_PLUGIN_EXPORT
//     void cppscheme2_plugin_init(Environment* env);
//
// Inside it, register primitives via scheme_register_primitive (below).  The
// plugin links cppscheme2_core, so every CPPSCHEME2_API function (make_integer,
// make_string, intern_symbol, ...) is available for building return values.

#include <string>
#include "scheme_export.h"
#include "AST.h"          // Value, BuiltinFn
#include "environment.h"  // Environment

// The C symbol a plugin must export, and a matching call type for the loader.
#define CPPSCHEME2_PLUGIN_INIT "cppscheme2_plugin_init"
extern "C" {
using cppscheme2_plugin_init_fn = void (*)(Environment*);
}

// Export decoration a plugin uses on its init entry (dllexport on Windows).
#ifdef _WIN32
#define CPPSCHEME2_PLUGIN_EXPORT __declspec(dllexport)
#else
#define CPPSCHEME2_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

// Core-side convenience for plugins: bind `name` to a primitive wrapping `fn`
// in `env`.  Implemented in plugin_api_impl.cpp so plugins need not reach
// Environment::bind / make_primitive directly.
CPPSCHEME2_API void scheme_register_primitive(Environment* env,
                                              const std::string& name,
                                              BuiltinFn fn);
