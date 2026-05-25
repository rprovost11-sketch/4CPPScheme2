#pragma once
// DLL visibility macros.  Used by all public API entry points.
// On Windows, CEKSCHEME_CORE_EXPORTS is defined only when building the DLL;
// including headers from outside the DLL gets the dllimport decoration.
#ifdef _WIN32
#  ifdef CEKSCHEME_CORE_EXPORTS
#    define CEKSCHEME_API __declspec(dllexport)
#  else
#    define CEKSCHEME_API __declspec(dllimport)
#  endif
#else
#  define CEKSCHEME_API __attribute__((visibility("default")))
#endif
