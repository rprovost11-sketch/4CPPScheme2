#pragma once
// plugin_loader.h -- load a native plugin shared library and run its init entry.
#include <string>

struct Environment;

// Load the plugin at `path` (.dll/.so) and call its cppscheme2_plugin_init
// entry with `env`.  Returns true on success; false if the file cannot be
// loaded or the init symbol is missing.  The library is intentionally left
// loaded for the process lifetime (its primitives live in the global env).
bool load_plugin(const std::string& path, Environment* env);
