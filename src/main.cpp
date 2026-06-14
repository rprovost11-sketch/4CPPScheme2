// main.cpp -- entry point.
// Direct port of pyscheme/__main__.py.
#include "Interpreter.h"
#include "Listener.h"
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <csignal>
// Forward-declare only what we need to avoid the <windows.h>/BOOLEAN conflict
// (same pattern as primitives/meta.cpp).
extern "C"
   {
   __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(
       void* hModule, wchar_t* lpFilename, unsigned long nSize);
   __declspec(dllimport) void* __stdcall GetCurrentProcess();
   __declspec(dllimport) int __stdcall TerminateProcess(void* hProcess, unsigned int uExitCode);
   }
static void _sigbreak_handler(int)
   {
   raise(SIGINT);
   }
#endif

// Split argv (excluding argv[0]) into library_paths + an optional positional
// target.  -L/--library-path takes one os-pathsep-separated list; -I takes one
// directory and may repeat; both contribute to library_paths in command-line
// order.  At most one positional target.  Port of __main__._parse_args.
static bool parse_args(int argc, char* argv[],
                       std::vector<std::string>& library_paths,
                       std::string& target, bool& have_target)
   {
#ifdef _WIN32
   const char sep = ';';
#else
   const char sep = ':';
#endif
   auto add_list = [&](const std::string& val)
   {
      size_t start = 0;
      while (true)
         {
         size_t pos = val.find(sep, start);
         std::string part = val.substr(start,
                                       pos == std::string::npos ? pos : pos - start);
         if (!part.empty())
            library_paths.push_back(part);
         if (pos == std::string::npos)
            break;
         start = pos + 1;
         }
   };
   for (int i = 1; i < argc; ++i)
      {
      std::string a = argv[i];
      if (a == "-L" || a == "--library-path")
         {
         if (i + 1 >= argc)
            {
            std::cerr << "cppscheme2: option " << a << " requires an argument\n";
            return false;
            }
         add_list(argv[++i]);
         }
      else if (a.rfind("-L=", 0) == 0)
         add_list(a.substr(3));
      else if (a.rfind("--library-path=", 0) == 0)
         add_list(a.substr(15));
      else if (a == "-I")
         {
         if (i + 1 >= argc)
            {
            std::cerr << "cppscheme2: option -I requires an argument\n";
            return false;
            }
         std::string d = argv[++i];
         if (!d.empty())
            library_paths.push_back(d);
         }
      else if (a.rfind("-I=", 0) == 0)
         {
         std::string d = a.substr(3);
         if (!d.empty())
            library_paths.push_back(d);
         }
      else if (a == "-" || a.empty() || a[0] != '-')
         {
         if (have_target)
            {
            std::cerr << "cppscheme2: unexpected extra argument: " << a << '\n';
            return false;
            }
         target = a;
         have_target = true;
         }
      else
         {
         std::cerr << "cppscheme2: unknown option: " << a << '\n';
         return false;
         }
      }
   return true;
   }

int main(int argc, char* argv[])
   {
#ifdef _WIN32
   signal(SIGBREAK, _sigbreak_handler);
#endif

   std::vector<std::string> library_paths;
   std::string target;
   bool have_target = false;
   if (!parse_args(argc, argv, library_paths, target, have_target))
      {
      std::cerr << "Usage: cppscheme2 [-L <dir" << char(
#ifdef _WIN32
                       ';'
#else
                       ':'
#endif
                       ) << "...>] [-I <dir>]... "
                   "[<directory> | <scheme-source-file>]\n";
      return 2;
      }

   // File mode: evaluate file, then hard-exit to bypass DLL static destructors.
   // DLL_PROCESS_DETACH static destructors (g_nursery et al.) are not safe to
   // run while GC objects are still live; TerminateProcess skips them entirely.
   if (have_target && std::filesystem::is_regular_file(target))
      {
         {
         Interpreter interp(library_paths);
         try
            {
            interp.evalFile(target);
            }
         catch (const std::exception& e)
            {
            std::cerr << "cppscheme2: " << e.what() << '\n';
            std::cout.flush();
#ifdef _WIN32
            TerminateProcess(GetCurrentProcess(), 1);
#else
            std::_Exit(1);
#endif
            }
         }
      std::cout.flush();
#ifdef _WIN32
      TerminateProcess(GetCurrentProcess(), 0);
#else
      std::_Exit(0);
#endif
      }

   Interpreter interp(library_paths);

   // Derive scheme-tests/ dir by walking up from the exe until we find a directory
   // that contains scheme-tests/ as a subdirectory.  This works for any build
   // configuration (Release, Debug, x64/Debug, etc.) without hardcoding depth.
   // Uses GetModuleFileNameW (Windows) so CWD and argv[0] ambiguity don't matter.
   std::string testdir, compliancedir, regressiondir, runsdir;
      {
      std::error_code ec;
#ifdef _WIN32
      wchar_t exeBuf[260] = {};
      GetModuleFileNameW(nullptr, exeBuf, 260);
      auto dir = std::filesystem::path(exeBuf).parent_path();
#else
      auto dir = std::filesystem::absolute(argv[0], ec).parent_path();
#endif
      std::filesystem::path scheme_tests;
      for (int i = 0; i < 8; ++i)
         {
         auto candidate = dir / "scheme-tests";
         if (std::filesystem::is_directory(candidate))
            {
            scheme_tests = candidate;
            break;
            }
         auto parent = dir.parent_path();
         if (parent == dir)
            break; // reached filesystem root
         dir = parent;
         }
      if (!scheme_tests.empty())
         {
         // The .log REPL-transcript suites live under scheme-tests/log-tests/
         // (grouped there to distinguish them from the application-tests suites).
         auto log_tests = scheme_tests / "log-tests";
         testdir = (log_tests / "feature-tests").string();
         compliancedir = (log_tests / "R7RS-Compliance-Tests").string();
         regressiondir = (log_tests / "regression-tests").string();
         runsdir = (scheme_tests / "runs").string();
         }
      }

   if (have_target)
      {
      if (std::filesystem::is_directory(target))
         {
         // Directory argument overrides testdir for one-off runs.
         std::filesystem::current_path(target);
         testdir = std::filesystem::current_path().string();
         // fall through to REPL
         }
      else
         {
         std::cerr << "cppscheme2: no such file or directory: " << target << '\n';
         return 1;
         }
      }

   Listener listener(
       &interp,
       testdir,
       "cppscheme2",
       "0.6.4",
       "Ron Provost/Longo",
       "https://github.com/rprovost11/cppscheme2",
       compliancedir,
       regressiondir,
       runsdir);
   listener.readEvalPrintLoop();

   return 0;
   }
