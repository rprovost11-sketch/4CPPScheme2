// common_dir.cpp -- locate the shared `pyscheme-cppscheme2-common` directory,
// the single home of the assets shared by pyScheme and cppScheme2: scheme-tests/,
// SRFI/, and the help/ docs+examples tree.  Mirrors pyscheme/common_dir.py.
//
// Resolution order:
//   1. $SCHEME_COMMON_DIR, if set and a directory.
//   2. A bounded walk-up from the executable's directory: at each ancestor, look
//      for a child named `pyscheme-cppscheme2-common`.  The first hit wins, which
//      yields subdir-before-sibling precedence for free and absorbs the
//      build/Release depth of the installed exe automatically.
//
// Returns "" when no common directory is found; every shared subdirectory is
// optional and callers degrade gracefully (no tests, no SRFI, no help) when one
// is absent.
#include "common_dir.h"

#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
// Forward-declare only what we need, matching the pattern in main.cpp/meta.cpp
// to avoid the <windows.h> macro conflicts.
extern "C" __declspec(dllimport) unsigned long __stdcall
GetModuleFileNameA(void* hModule, char* lpFilename, unsigned long nSize);
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

static const char* COMMON_DIR_NAME = "pyscheme-cppscheme2-common";
static const int MAX_WALK_UP = 8;   // exe sits under build/Release/, so allow depth

// Absolute path of the running executable, or "" if it cannot be determined.
static std::string self_exe_path()
   {
   char buf[4096];
#ifdef _WIN32
   unsigned long len = GetModuleFileNameA(nullptr, buf, (unsigned long)sizeof(buf));
   if (len == 0 || len >= sizeof(buf))
      return std::string();
   return std::string(buf, len);
#else
   ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf));
   if (len <= 0 || (size_t)len >= sizeof(buf))
      return std::string();
   return std::string(buf, (size_t)len);
#endif
   }

std::string find_common_root()
   {
   if (const char* env = std::getenv("SCHEME_COMMON_DIR"); env && env[0])
      {
      std::error_code ec;
      if (fs::is_directory(env, ec))
         return std::string(env);
      return std::string();
      }

   std::string exe = self_exe_path();
   if (exe.empty())
      return std::string();

   std::error_code ec;
   fs::path cur = fs::path(exe).parent_path();         // the exe's directory
   for (int levels = 0; levels <= MAX_WALK_UP; ++levels)
      {
      fs::path candidate = cur / COMMON_DIR_NAME;
      if (fs::is_directory(candidate, ec))
         return candidate.string();
      fs::path parent = cur.parent_path();
      if (parent == cur)                                // reached the filesystem root
         break;
      cur = parent;
      }
   return std::string();
   }

std::string common_subdir(const std::string& name)
   {
   std::string root = find_common_root();
   if (root.empty())
      return std::string();
   std::error_code ec;
   fs::path path = fs::path(root) / name;
   if (fs::is_directory(path, ec))
      return path.string();
   return std::string();
   }
