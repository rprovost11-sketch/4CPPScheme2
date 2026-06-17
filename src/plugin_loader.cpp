// plugin_loader.cpp -- dlopen/LoadLibrary a native plugin and call its init.
//
// Deliberately includes NO Scheme headers: windows.h's BOOLEAN typedef clashes
// with AST.h's BOOLEAN enum tag, so this TU stays free of AST.h.  It needs only
// an opaque Environment* (forward-declared in plugin_loader.h).  The init-entry
// name must match scheme_plugin_api.h's CPPSCHEME2_PLUGIN_INIT.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "plugin_loader.h"

namespace {
constexpr const char* PLUGIN_INIT_SYMBOL = "cppscheme2_plugin_init";
using plugin_init_fn = void (*)(Environment*);
}  // namespace

bool load_plugin(const std::string& path, Environment* env)
   {
#ifdef _WIN32
   HMODULE h = LoadLibraryA(path.c_str());
   if (!h)
      return false;
   auto fn = reinterpret_cast<plugin_init_fn>(
       reinterpret_cast<void*>(GetProcAddress(h, PLUGIN_INIT_SYMBOL)));
   if (!fn)
      return false;            // leave the module loaded; nothing to register
   fn(env);
   return true;
#else
   void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
   if (!h)
      return false;
   auto fn = reinterpret_cast<plugin_init_fn>(dlsym(h, PLUGIN_INIT_SYMBOL));
   if (!fn)
      return false;
   fn(env);
   return true;
#endif
   }
