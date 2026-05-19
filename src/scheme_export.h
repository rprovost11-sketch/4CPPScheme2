#pragma once
// scheme_export.h — DLL export/import macro for cekscheme_core shared library.

#ifdef _WIN32
   #ifdef CEKSCHEME_CORE_EXPORTS
      #define SCHEME_API __declspec(dllexport)
   #else
      #define SCHEME_API __declspec(dllimport)
   #endif
#else
   #define SCHEME_API __attribute__((visibility("default")))
#endif
