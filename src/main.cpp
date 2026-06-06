// main.cpp -- entry point.
// Direct port of pyscheme/__main__.py.
#include "Interpreter.h"
#include "Listener.h"
#include <filesystem>
#include <iostream>

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

int main(int argc, char* argv[])
   {
#ifdef _WIN32
   signal(SIGBREAK, _sigbreak_handler);
#endif

   if (argc > 2)
      {
      std::cerr << "Usage: cppscheme2 [<directory> | <scheme-source-file>]\n";
      return 2;
      }

   // File mode: evaluate file, then hard-exit to bypass DLL static destructors.
   // DLL_PROCESS_DETACH static destructors (g_nursery et al.) are not safe to
   // run while GC objects are still live; TerminateProcess skips them entirely.
   if (argc == 2 && std::filesystem::is_regular_file(argv[1]))
      {
      std::string target = argv[1];
         {
         Interpreter interp;
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

   Interpreter interp;

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
         testdir = (scheme_tests / "feature-tests").string();
         compliancedir = (scheme_tests / "R7RS-Compliance-Tests").string();
         regressiondir = (scheme_tests / "regression-tests").string();
         runsdir = (scheme_tests / "runs").string();
         }
      }

   if (argc == 2)
      {
      std::string target = argv[1];
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
       "0.5.7",
       "Ron Provost/Longo",
       "https://github.com/rprovost11/cppscheme2",
       compliancedir,
       regressiondir,
       runsdir);
   listener.readEvalPrintLoop();

   return 0;
   }
