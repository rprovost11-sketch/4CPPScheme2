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
