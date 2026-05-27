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
extern "C" {
    __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(
        void* hModule, wchar_t* lpFilename, unsigned long nSize);
    __declspec(dllimport) void* __stdcall GetCurrentProcess();
    __declspec(dllimport) int   __stdcall TerminateProcess(void* hProcess, unsigned int uExitCode);
}
static void _sigbreak_handler(int) { raise(SIGINT); }
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    signal(SIGBREAK, _sigbreak_handler);
#endif

    if (argc > 2) {
        std::cerr << "Usage: cppscheme2 [<directory> | <scheme-source-file>]\n";
        return 2;
    }

    // File mode: evaluate file, then hard-exit to bypass DLL static destructors.
    // DLL_PROCESS_DETACH static destructors (g_nursery et al.) are not safe to
    // run while GC objects are still live; TerminateProcess skips them entirely.
    if (argc == 2 && std::filesystem::is_regular_file(argv[1])) {
        std::string target = argv[1];
        {
            Interpreter interp;
            try {
                interp.evalFile(target);
            } catch (const std::exception& e) {
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

    std::string testdir = "feature-tests";
    if (argc == 2) {
        std::string target = argv[1];
        if (std::filesystem::is_directory(target)) {
            std::filesystem::current_path(target);
            testdir = ".";
            // fall through to REPL
        } else {
            std::cerr << "cppscheme2: no such file or directory: " << target << '\n';
            return 1;
        }
    }

    // Derive compliance dir: 3 levels up from the exe, then R7RS-Compliance-Tests.
    // Use GetModuleFileNameW (Windows) so argv[0] ambiguity doesn't break the path.
    // Mirrors pyscheme's os.path.abspath(__file__) approach.
    std::string compliancedir;
    {
        std::error_code ec;
#ifdef _WIN32
        wchar_t exeBuf[260] = {};  // 260 == MAX_PATH
        GetModuleFileNameW(nullptr, exeBuf, 260);
        auto exeDir = std::filesystem::path(exeBuf).parent_path();
#else
        auto exeDir = std::filesystem::absolute(argv[0], ec).parent_path();
#endif
        auto cdir = std::filesystem::weakly_canonical(
            exeDir / ".." / ".." / ".." / "R7RS-Compliance-Tests", ec);
        if (!ec && std::filesystem::is_directory(cdir))
            compliancedir = cdir.string();
    }

    Listener listener(
        &interp,
        testdir,
        "cppscheme2",
        "0.4.11",
        "Ron Provost/Longo",
        "https://github.com/rprovost11/cppscheme2",
        compliancedir);
    listener.readEvalPrintLoop();

    return 0;
}
