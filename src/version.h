// version.h -- single source of truth for the cppScheme2 version string.
//
// To cut a release, bump this ONE line (and keep it lockstep with pyScheme's
// pyscheme/__init__.py __version__).  It was previously duplicated as "0.8.1"
// string literals in main.cpp; centralising it here lets the (interpreter-version)
// primitive report the same value the banner/Listener identity uses.
//
// Consumed by: main.cpp (Listener identity / banner) and primitives/meta.cpp
// (the (interpreter-version) primitive).
#ifndef CPPSCHEME2_VERSION_H
#define CPPSCHEME2_VERSION_H

inline constexpr const char* CPPSCHEME2_VERSION = "0.8.1";

#endif  // CPPSCHEME2_VERSION_H
