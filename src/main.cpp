// main.cpp -- entry point.
// Direct port of pyscheme/__main__.py.
#include "Interpreter.h"
#include "Listener.h"
#include <cstdlib>
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
                       std::string& target, bool& have_target,
                       std::vector<std::string>& eval_exprs, bool& have_eval,
                       std::string& scheme_tests, bool& have_scheme_tests,
                       bool& no_rc)
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
      else if (a == "-e" || a == "--evaluate")
         {
         if (i + 1 >= argc)
            {
            std::cerr << "cppscheme2: option " << a << " requires an argument\n";
            return false;
            }
         eval_exprs.push_back(argv[++i]);
         have_eval = true;
         }
      else if (a.rfind("-e=", 0) == 0)
         {
         eval_exprs.push_back(a.substr(3));
         have_eval = true;
         }
      else if (a.rfind("--evaluate=", 0) == 0)
         {
         eval_exprs.push_back(a.substr(11));
         have_eval = true;
         }
      else if (a == "-T" || a == "--scheme-tests")
         {
         if (i + 1 >= argc)
            {
            std::cerr << "cppscheme2: option " << a << " requires an argument\n";
            return false;
            }
         scheme_tests = argv[++i];
         have_scheme_tests = true;
         }
      else if (a.rfind("-T=", 0) == 0)
         {
         scheme_tests = a.substr(3);
         have_scheme_tests = true;
         }
      else if (a.rfind("--scheme-tests=", 0) == 0)
         {
         scheme_tests = a.substr(15);
         have_scheme_tests = true;
         }
      else if (a == "--no-rc")
         no_rc = true;
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
   if (have_eval && have_target)
      {
      std::cerr << "cppscheme2: -e/--evaluate cannot be combined with a file or directory\n";
      return false;
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
   std::vector<std::string> eval_exprs;
   bool have_eval = false;
   std::string scheme_tests_cli;
   bool have_scheme_tests = false;
   bool no_rc = false;
   if (!parse_args(argc, argv, library_paths, target, have_target, eval_exprs,
                   have_eval, scheme_tests_cli, have_scheme_tests, no_rc))
      {
      std::cerr << "Usage: cppscheme2 [-L <dir" << char(
#ifdef _WIN32
                       ';'
#else
                       ':'
#endif
                       ) << "...>] [-I <dir>]... [-T <scheme-tests-dir>] "
                   "[-e <expr>]... [--no-rc] [<directory> | <scheme-source-file>]\n";
      return 2;
      }

   // Resolve the scheme-tests root: the -T/--scheme-tests option overrides the
   // SCHEME_TESTS_DIR environment variable; the ]scheme-tests listener command
   // can override both at runtime.  Nothing is hardcoded -- if none is given the
   // test commands explain how to set it.
   std::string scheme_tests_dir;
   std::string scheme_tests_source = "unset";
   if (have_scheme_tests)
      {
      scheme_tests_dir = scheme_tests_cli;
      scheme_tests_source = "--scheme-tests option";
      }
   else if (const char* env = std::getenv("SCHEME_TESTS_DIR"); env && env[0])
      {
      scheme_tests_dir = env;
      scheme_tests_source = "SCHEME_TESTS_DIR env";
      }

   // File mode: evaluate file, then hard-exit to bypass DLL static destructors.
   // DLL_PROCESS_DETACH static destructors (g_nursery et al.) are not safe to
   // run while GC objects are still live; TerminateProcess skips them entirely.
   if (have_target && std::filesystem::is_regular_file(target))
      {
         {
         Interpreter interp(library_paths, !no_rc);
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

   Interpreter interp(library_paths, !no_rc);

   // -e/--evaluate: evaluate each expression as a REPL transcript, then exit.
   // The banner is suppressed so the first line is the '>>> ' echo.  Hard-exit
   // (like file mode) to skip DLL static destructors while GC objects are live.
   if (have_eval)
      {
      int status = 0;
         {
         Listener listener(
             &interp, "cppscheme2", "0.8.0", "Ron Provost/Longo",
             "https://github.com/rprovost11/cppscheme2", scheme_tests_dir,
             scheme_tests_source, /*show_banner=*/false);
         status = listener.eval_and_exit(eval_exprs);
         }
      std::cout.flush();
#ifdef _WIN32
      TerminateProcess(GetCurrentProcess(), (unsigned)status);
#else
      std::_Exit(status);
#endif
      }

   if (have_target)
      {
      if (std::filesystem::is_directory(target))
         {
         // Directory argument: run the REPL rooted there.
         std::filesystem::current_path(target);
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
       "cppscheme2",
       "0.8.0",
       "Ron Provost/Longo",
       "https://github.com/rprovost11/cppscheme2",
       scheme_tests_dir,
       scheme_tests_source);
   listener.readEvalPrintLoop();

   return 0;
   }
