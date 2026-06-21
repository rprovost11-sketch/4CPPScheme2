// process_exec.h -- blocking subprocess execution for (run-process ...).
//
// Kept FREE of Scheme/AST headers (only std types) so process_exec.cpp can include
// <windows.h> without the BOOLEAN-typedef-vs-AST.h-enum clash (same isolation
// rationale as plugin_loader.{h,cpp}).  meta.cpp calls run_process_blocking and
// wraps the result into Scheme values.
#ifndef CPPSCHEME2_PROCESS_EXEC_H
#define CPPSCHEME2_PROCESS_EXEC_H

#include <string>
#include <vector>

struct ProcessResult
   {
   bool        launched;    // false if the child could not be started
   int         exit_code;   // exit status; negated signal number on POSIX signal-kill
   std::string out;         // captured stdout (raw bytes)
   std::string err;         // captured stderr (raw bytes)
   std::string error;       // human-readable launch-failure reason when !launched
   };

// Run argv directly (argv[0] = program, searched on PATH; NO shell, so arguments
// are passed verbatim and are injection-safe).  Blocks until the child exits,
// draining stdout/stderr concurrently to avoid pipe-buffer deadlock.  If
// stdin_data != nullptr it is written to the child's stdin, which is then closed
// (child sees EOF); otherwise stdin is an immediately-closed empty pipe.
ProcessResult run_process_blocking(const std::vector<std::string>& argv,
                                   const std::string* stdin_data);

#endif  // CPPSCHEME2_PROCESS_EXEC_H
