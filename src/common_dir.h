#ifndef CPPSCHEME2_COMMON_DIR_H
#define CPPSCHEME2_COMMON_DIR_H

#include "scheme_export.h"
#include <string>

// Locate the shared `pyscheme-cppscheme2-common` directory (see common_dir.cpp
// for the resolution order).  Returns "" when no common directory is found.
CPPSCHEME2_API std::string find_common_root();

// Return <root>/<name> if both the common root and that subdirectory exist,
// else "".  Used to resolve the individual optional shared trees
// (scheme-tests/, SRFI/, help/).
CPPSCHEME2_API std::string common_subdir(const std::string& name);

#endif  // CPPSCHEME2_COMMON_DIR_H
