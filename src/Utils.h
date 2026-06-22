#pragma once
// Utils.h -- Listener-side utility helpers.
// Direct port of pyscheme/Utils.py.
#include "scheme_export.h"
#include <iosfwd>
#include <string>
#include <vector>

// ── File utilities ────────────────────────────────────────────────────────────
// Port of Utils.py retrieveFileList.
// Returns a sorted list of absolute .log file paths in dirname.
CPPSCHEME2_API std::vector<std::string> retrieveFileList(const std::string& dirname);

// ── columnize ─────────────────────────────────────────────────────────────────
// Port of Utils.py columnize.
// Prints lst as a compact multi-column table to out (nullptr -> std::cout).
// item_color: ANSI escape applied to all items; "" for none.
// item_colors: per-item ANSI codes; when non-null overrides item_color.
CPPSCHEME2_API void columnize(const std::vector<std::string>& lst,
                              int display_width = 80,
                              std::ostream* out = nullptr,
                              const std::string& item_color = "",
                              const std::vector<std::string>* item_colors = nullptr);

// ── ParenState ────────────────────────────────────────────────────────────────
// Port of Utils.py ParenState.
// Net paren depth + in_string flag after scanning a chunk of text.
// stack holds each unclosed delimiter ('(' or '[') in open order.

struct CPPSCHEME2_API ParenState
   {
   int depth;
   bool in_string;
   std::vector<char> stack;
   };

// Port of Utils.py paren_state.
// Scans text ignoring string contents and ; comments.
CPPSCHEME2_API ParenState paren_state(const std::string& text);

// ── writeln_multiFile ─────────────────────────────────────────────────────────
// Port of Utils.py writeln_multiFile.
// Prints output_string + newline to every stream in file_list.
// A nullptr entry in file_list writes to std::cout.
CPPSCHEME2_API void writeln_multiFile(const std::string& output_string,
                                      const std::vector<std::ostream*>& file_list,
                                      bool flush = false);

// ── Session-log parsing + match semantics ──────────────────────────────────────
// A second, free-standing implementation of the .log parser and the test match
// semantics, used to back the (parse-log-file ...) and (log-match? ...) primitives
// (and through them the Scheme-side universal interpreter differ, which treats a
// .log file as a reference interpreter).  Listener keeps its OWN copy
// (Listener::parse_log / sessionLog_test) for the in-process test runner; these are
// deliberately independent so differ work does not disturb the existing harness.
// The two must stay behaviourally identical.

// One parsed log entry: expression, output, return value, error, fold-case flag.
struct CPPSCHEME2_API LogEntry
   {
   std::string expr;
   std::string output;
   std::string retval;
   std::string error;
   bool fold_case = false;
   };

// Parse a session log into a list of LogEntry structs.  Each entry begins with a
// '>>> ' line; '... ' lines continue the expression; lines before '==> ' are
// output; '==> ' gives the return value, '%%% ' the error; '#!fold-case' /
// '#!no-fold-case' toggle case folding for following entries.
CPPSCHEME2_API std::vector<LogEntry> parse_log(const std::string& text);

// True if actual matches expected, honouring 'X or ==> Y' alternatives.
CPPSCHEME2_API bool log_match_retval(const std::string& actual,
                                     const std::string& expected);

// Per-channel result of comparing one interpreter cycle's actual outcome to the
// expected (golden) one.
struct CPPSCHEME2_API LogMatch
   {
   bool output_ok;
   bool retval_ok;
   bool error_ok;
   };

// Apply the .log test match semantics.  '%%% *' / '%%% %any-error%' accept any
// raised error; '%%% %optional-error%' models R7RS "it is an error" (passes whether
// or not an error is raised); a timeout always fails.
CPPSCHEME2_API LogMatch log_match(const std::string& expected_output,
                                  const std::string& expected_retval,
                                  const std::string& expected_error,
                                  const std::string& actual_output,
                                  const std::string& actual_retval,
                                  const std::string& actual_error,
                                  bool timed_out);
