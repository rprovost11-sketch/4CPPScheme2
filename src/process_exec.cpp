// process_exec.cpp -- blocking subprocess execution (Windows + POSIX).
//
// NO Scheme/AST headers here so we can include <windows.h> freely (its BOOLEAN
// typedef clashes with AST.h's BOOLEAN enum tag).  See process_exec.h.
//
// All three std streams are pumped on separate threads so the main thread is free
// to wait on the child with a timeout: on Windows via WaitForSingleObject, on POSIX
// via a waitpid(WNOHANG) poll loop.  Killing the child closes its pipe ends, which
// unblocks (and ends) the reader threads.

#include "process_exec.h"

#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <spawn.h>
#include <csignal>
#include <cstring>
extern char** environ;
#endif

#ifdef _WIN32

// Quote one argv element per the MSVCRT command-line parsing rules so the child's
// C runtime reconstructs exactly this argument (the Windows analogue of "no shell").
static std::string win_quote(const std::string& arg)
   {
   if (!arg.empty() &&
       arg.find_first_of(" \t\n\v\"") == std::string::npos)
      return arg;  // no quoting needed
   std::string out = "\"";
   for (size_t i = 0; ; ++i)
      {
      size_t backslashes = 0;
      while (i < arg.size() && arg[i] == '\\') { ++backslashes; ++i; }
      if (i == arg.size())
         {
         out.append(backslashes * 2, '\\');  // escape trailing backslashes
         break;
         }
      if (arg[i] == '"')
         {
         out.append(backslashes * 2 + 1, '\\');  // escape backslashes + the quote
         out.push_back('"');
         }
      else
         {
         out.append(backslashes, '\\');
         out.push_back(arg[i]);
         }
      }
   out.push_back('"');
   return out;
   }

static void read_all(HANDLE h, std::string& into)
   {
   char buf[65536];
   DWORD n = 0;
   while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
      into.append(buf, n);
   }

ProcessResult run_process_blocking(const std::vector<std::string>& argv,
                                   const std::string* stdin_data,
                                   double timeout_secs)
   {
   ProcessResult r{false, false, 0, "", "", ""};
   if (argv.empty()) { r.error = "run-process: empty argv"; return r; }

   std::string cmdline;
   for (size_t i = 0; i < argv.size(); ++i)
      {
      if (i) cmdline.push_back(' ');
      cmdline += win_quote(argv[i]);
      }

   SECURITY_ATTRIBUTES sa{};
   sa.nLength = sizeof(sa);
   sa.bInheritHandle = TRUE;

   HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr,
          errR = nullptr, errW = nullptr;
   if (!CreatePipe(&inR, &inW, &sa, 0) ||
       !CreatePipe(&outR, &outW, &sa, 0) ||
       !CreatePipe(&errR, &errW, &sa, 0))
      { r.error = "run-process: CreatePipe failed"; return r; }
   SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
   SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
   SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

   STARTUPINFOA si{};
   si.cb = sizeof(si);
   si.dwFlags = STARTF_USESTDHANDLES;
   si.hStdInput = inR;
   si.hStdOutput = outW;
   si.hStdError = errW;
   PROCESS_INFORMATION pi{};

   std::vector<char> cmdbuf(cmdline.begin(), cmdline.end());
   cmdbuf.push_back('\0');
   BOOL ok = CreateProcessA(nullptr, cmdbuf.data(), nullptr, nullptr,
                            TRUE, 0, nullptr, nullptr, &si, &pi);
   CloseHandle(inR); CloseHandle(outW); CloseHandle(errW);
   if (!ok)
      {
      CloseHandle(inW); CloseHandle(outR); CloseHandle(errR);
      r.error = "run-process: CreateProcess failed for '" + argv[0] + "'";
      return r;
      }

   // All I/O on threads so the main thread can wait with a timeout.
   std::thread writer([&]
      {
      if (stdin_data && !stdin_data->empty())
         {
         size_t off = 0;
         while (off < stdin_data->size())
            {
            DWORD wrote = 0;
            if (!WriteFile(inW, stdin_data->data() + off,
                           (DWORD)(stdin_data->size() - off), &wrote, nullptr))
               break;
            off += wrote;
            }
         }
      CloseHandle(inW);
      });
   std::thread t_out([&] { read_all(outR, r.out); });
   std::thread t_err([&] { read_all(errR, r.err); });

   DWORD wait_ms = (timeout_secs > 0)
                       ? (DWORD)(timeout_secs * 1000.0 + 0.5) : INFINITE;
   if (WaitForSingleObject(pi.hProcess, wait_ms) == WAIT_TIMEOUT)
      {
      r.timed_out = true;
      TerminateProcess(pi.hProcess, 1);   // closes child pipe ends -> readers end
      WaitForSingleObject(pi.hProcess, INFINITE);
      }

   writer.join(); t_out.join(); t_err.join();
   CloseHandle(outR); CloseHandle(errR);

   DWORD code = 0;
   GetExitCodeProcess(pi.hProcess, &code);
   CloseHandle(pi.hProcess);
   CloseHandle(pi.hThread);

   r.launched = true;
   r.exit_code = (int)code;
   return r;
   }

#else  // POSIX

static void read_all(int fd, std::string& into)
   {
   char buf[65536];
   ssize_t n;
   while ((n = read(fd, buf, sizeof(buf))) > 0)
      into.append(buf, (size_t)n);
   }

ProcessResult run_process_blocking(const std::vector<std::string>& argv,
                                   const std::string* stdin_data,
                                   double timeout_secs)
   {
   ProcessResult r{false, false, 0, "", "", ""};
   if (argv.empty()) { r.error = "run-process: empty argv"; return r; }

   int inP[2], outP[2], errP[2];
   if (pipe(inP) != 0) { r.error = "run-process: pipe failed"; return r; }
   if (pipe(outP) != 0) { close(inP[0]); close(inP[1]); r.error = "run-process: pipe failed"; return r; }
   if (pipe(errP) != 0)
      { close(inP[0]); close(inP[1]); close(outP[0]); close(outP[1]);
        r.error = "run-process: pipe failed"; return r; }

   posix_spawn_file_actions_t fa;
   posix_spawn_file_actions_init(&fa);
   posix_spawn_file_actions_adddup2(&fa, inP[0], 0);
   posix_spawn_file_actions_adddup2(&fa, outP[1], 1);
   posix_spawn_file_actions_adddup2(&fa, errP[1], 2);
   posix_spawn_file_actions_addclose(&fa, inP[1]);
   posix_spawn_file_actions_addclose(&fa, outP[0]);
   posix_spawn_file_actions_addclose(&fa, errP[0]);

   std::vector<char*> cargv;
   cargv.reserve(argv.size() + 1);
   for (const std::string& a : argv)
      cargv.push_back(const_cast<char*>(a.c_str()));
   cargv.push_back(nullptr);

   pid_t pid = 0;
   int rc = posix_spawnp(&pid, argv[0].c_str(), &fa, nullptr, cargv.data(), environ);
   posix_spawn_file_actions_destroy(&fa);
   close(inP[0]); close(outP[1]); close(errP[1]);
   if (rc != 0)
      {
      close(inP[1]); close(outP[0]); close(errP[0]);
      r.error = "run-process: posix_spawnp failed for '" + argv[0] + "'";
      return r;
      }

   std::thread writer([&]
      {
      if (stdin_data && !stdin_data->empty())
         {
         size_t off = 0;
         while (off < stdin_data->size())
            {
            ssize_t w = write(inP[1], stdin_data->data() + off, stdin_data->size() - off);
            if (w <= 0) break;
            off += (size_t)w;
            }
         }
      close(inP[1]);
      });
   std::thread t_out([&] { read_all(outP[0], r.out); });
   std::thread t_err([&] { read_all(errP[0], r.err); });

   int status = 0;
   if (timeout_secs > 0)
      {
      auto start = std::chrono::steady_clock::now();
      for (;;)
         {
         pid_t w = waitpid(pid, &status, WNOHANG);
         if (w == pid) break;                      // child exited
         double elapsed = std::chrono::duration<double>(
             std::chrono::steady_clock::now() - start).count();
         if (elapsed >= timeout_secs)
            {
            r.timed_out = true;
            kill(pid, SIGKILL);                    // closes pipe ends -> readers end
            waitpid(pid, &status, 0);
            break;
            }
         std::this_thread::sleep_for(std::chrono::milliseconds(10));
         }
      }
   else
      waitpid(pid, &status, 0);

   writer.join(); t_out.join(); t_err.join();
   close(outP[0]); close(errP[0]);

   r.launched = true;
   if (WIFEXITED(status))
      r.exit_code = WEXITSTATUS(status);
   else if (WIFSIGNALED(status))
      r.exit_code = -WTERMSIG(status);
   else
      r.exit_code = -1;
   return r;
   }

#endif
