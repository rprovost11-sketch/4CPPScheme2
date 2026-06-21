// dir_list.cpp -- native directory listing (Windows + POSIX).
//
// NO Scheme/AST headers here so we can include <windows.h> freely.  See dir_list.h.

#include "dir_list.h"

#ifdef _WIN32
#include <windows.h>

bool list_directory(const std::string& path, std::vector<std::string>& out)
   {
   // Build the search pattern "<path>\*".
   std::string pat = path;
   if (!pat.empty() && pat.back() != '/' && pat.back() != '\\')
      pat.push_back('\\');
   pat.push_back('*');

   // UTF-8 -> UTF-16 for the wide FindFirstFileW (handles Unicode filenames).
   int wlen = MultiByteToWideChar(CP_UTF8, 0, pat.c_str(), -1, nullptr, 0);
   if (wlen <= 0) return false;
   std::wstring wpat(wlen, L'\0');
   MultiByteToWideChar(CP_UTF8, 0, pat.c_str(), -1, &wpat[0], wlen);

   WIN32_FIND_DATAW fd;
   HANDLE h = FindFirstFileW(wpat.c_str(), &fd);
   if (h == INVALID_HANDLE_VALUE) return false;
   do
      {
      const wchar_t* w = fd.cFileName;
      if ((w[0] == L'.' && w[1] == L'\0') ||
          (w[0] == L'.' && w[1] == L'.' && w[2] == L'\0'))
         continue;  // skip "." and ".."
      int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
      if (n <= 0) continue;
      std::string name(n, '\0');
      int wrote = WideCharToMultiByte(CP_UTF8, 0, w, -1, &name[0], n, nullptr, nullptr);
      if (wrote > 0)
         {
         name.resize(wrote - 1);  // drop the trailing NUL
         out.push_back(name);
         }
      }
   while (FindNextFileW(h, &fd));
   FindClose(h);
   return true;
   }

#else  // POSIX
#include <dirent.h>
#include <cstring>

bool list_directory(const std::string& path, std::vector<std::string>& out)
   {
   DIR* d = opendir(path.c_str());
   if (!d) return false;
   struct dirent* e;
   while ((e = readdir(d)) != nullptr)
      {
      const char* nm = e->d_name;
      if (std::strcmp(nm, ".") == 0 || std::strcmp(nm, "..") == 0)
         continue;
      out.push_back(nm);
      }
   closedir(d);
   return true;
   }

#endif
