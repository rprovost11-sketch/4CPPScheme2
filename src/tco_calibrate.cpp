// tco_calibrate.cpp -- spawn-a-child heap-OOM (broken-TCO) threshold finder.
// Mirrors bench/calibrate_tco.ps1.  Kept in its own TU so <windows.h> does not
// pollute the listener's preprocessor state (windows.h defines TRUE/FALSE/ERROR/
// VOID/min/max/... which collide with the interpreter headers).
#include "tco_calibrate.h"

#include <filesystem>
#include <fstream>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace
   {
   // Run the child interpreter on (grow N).  true = completed (exit 0 + sentinel);
   // false = crashed / OOM / timed out.  Mirrors calibrate_tco.ps1 Test-N.
   bool calib_test_n(const std::wstring& exePath, long long n, int timeoutSec,
                     const fs::path& scmPath, const fs::path& outPath)
      {
         {
         std::ofstream f(scmPath, std::ios::out | std::ios::trunc);
         f << "(define (grow k) (if (= k 0) 0 (+ 1 (grow (- k 1)))))\n";
         f << "(grow " << n << ")\n";
         f << "(display \"CALIBRATE_OK\") (newline)\n";
         }

      SECURITY_ATTRIBUTES sa;
      sa.nLength = sizeof(sa);
      sa.lpSecurityDescriptor = nullptr;
      sa.bInheritHandle = TRUE;
      HANDLE hOut = CreateFileW(outPath.wstring().c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (hOut == INVALID_HANDLE_VALUE)
         return false;

      STARTUPINFOW si;
      ZeroMemory(&si, sizeof(si));
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESTDHANDLES;
      si.hStdInput = nullptr;
      si.hStdOutput = hOut;
      si.hStdError = hOut;

      std::wstring cmd = L"\"" + exePath + L"\" \"" + scmPath.wstring() + L"\"";
      std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
      cmdBuf.push_back(L'\0');

      PROCESS_INFORMATION pi;
      ZeroMemory(&pi, sizeof(pi));
      BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
      CloseHandle(hOut);
      if (!ok)
         return false;

      DWORD waited = WaitForSingleObject(pi.hProcess, (DWORD)(timeoutSec * 1000));
      bool success = false;
      if (waited == WAIT_TIMEOUT)
         {
         TerminateProcess(pi.hProcess, 1);
         WaitForSingleObject(pi.hProcess, 5000);
         }
      else
         {
         DWORD code = 1;
         GetExitCodeProcess(pi.hProcess, &code);
         success = (code == 0);
         }
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);

      if (success)
         {
         std::ifstream rf(outPath, std::ios::in | std::ios::binary);
         std::stringstream ss;
         ss << rf.rdbuf();
         success = ss.str().find("CALIBRATE_OK") != std::string::npos;
         }
      return success;
      }
   } // namespace

long long calibrate_tco_threshold(std::ostream& out)
   {
   wchar_t exeBuf[MAX_PATH] = {};
   if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH) == 0)
      return 0;
   std::wstring exePath(exeBuf);

   DWORD pid = GetCurrentProcessId();
   std::string tag = "cppscheme2_calib_" + std::to_string((unsigned long)pid);
   fs::path scmPath = fs::temp_directory_path() / (tag + ".scm");
   fs::path outPath = fs::temp_directory_path() / (tag + ".out");

   const long long START = 100000;
   const long long MAXN = 200000000;
   const int TIMEOUT = 120;

   auto cleanup = [&]()
   {
      std::error_code ec;
      fs::remove(scmPath, ec);
      fs::remove(outPath, ec);
   };

   out << "; compliance-slow: calibrating this machine's heap-OOM (broken-TCO)\n"
          ";   threshold by spawning child interpreters (slow, memory-heavy)...\n"
       << std::flush;

   // 1) Exponential ramp: double N until the proxy fails (or we hit MaxN).
   long long lo = 0, hi = 0, n = START;
   while (true)
      {
      out << ";   ramp   N = " << n << " ..." << std::flush;
      if (calib_test_n(exePath, n, TIMEOUT, scmPath, outPath))
         {
         out << " ok\n" << std::flush;
         lo = n;
         if (n >= MAXN)
            {
            out << "; reached the ramp cap with no failure; soaking at -I:" << MAXN
                << ".\n" << std::flush;
            cleanup();
            return MAXN;
            }
         long long doubled = n * 2;
         n = (doubled < MAXN) ? doubled : MAXN;
         }
      else
         {
         out << " FAILED (oom/crash/timeout)\n" << std::flush;
         hi = n;
         break;
         }
      }

   // 2) Binary search between lo (last ok) and hi (first fail), ~2% precision.
   while (true)
      {
      long long span = hi - lo;
      long long tol = lo / 50;
      if (tol < 1)
         tol = 1;
      if (span <= tol)
         break;
      long long mid = lo + span / 2;
      out << ";   bisect N = " << mid << " ..." << std::flush;
      if (calib_test_n(exePath, mid, TIMEOUT, scmPath, outPath))
         {
         out << " ok\n" << std::flush;
         lo = mid;
         }
      else
         {
         out << " FAILED\n" << std::flush;
         hi = mid;
         }
      }

   cleanup();
   long long iters = (hi / 1000000 + 1) * 1000000;
   out << "; broken TCO dies around ~" << hi
       << " iters here; soaking compliance at -I:" << iters << ".\n"
       << std::flush;
   return iters;
   }

#else // !_WIN32

long long calibrate_tco_threshold(std::ostream&)
   {
   return 0; // calibration is Windows-only (spawns child processes)
   }

#endif // _WIN32
