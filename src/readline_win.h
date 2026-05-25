#pragma once
// readline_win.h -- Windows console readline replacement using _getwch().
// Direct port of pyscheme/readline_win.py. Windows-only.
#ifdef _WIN32
#include "scheme_export.h"
#include <stdexcept>
#include <string>
#include <vector>

// Raised when the user presses Ctrl-D/Ctrl-Z on an empty line (port of EOFError).
struct CEKSCHEME_API ReadlineEOFError : std::exception {
    const char* what() const noexcept override { return "readline EOF"; }
};

// Raised when the user presses Ctrl-C (port of KeyboardInterrupt).
struct CEKSCHEME_API ReadlineInterruptError : std::exception {
    const char* what() const noexcept override { return "readline interrupt"; }
};

// Read one line of input with full line editing and history navigation.
// prompt:               displayed before the first (or only) input line.
// continuation_prompt:  displayed before subsequent lines of a multi-line entry.
// prefill:              pre-populates the buffer (used for auto-indent).
// Throws ReadlineEOFError on Ctrl-D/Ctrl-Z when buffer is empty.
// Throws ReadlineInterruptError on Ctrl-C.
CEKSCHEME_API std::string readline_win_input_line(
    const std::string& prompt               = "",
    const std::string& continuation_prompt  = "... ",
    const std::string& prefill              = "");

// Append entry to history (skips empty and exact duplicates of most recent).
CEKSCHEME_API void readline_win_add_history(const std::string& entry);

// Set maximum number of history entries retained.
CEKSCHEME_API void readline_win_set_history_length(int n);

// Load history from file; silently ignores a missing file.
// Embedded newlines are stored in the file as the two-character sequence \n
// and decoded on load.
CEKSCHEME_API void readline_win_read_history_file(const std::string& path);

// Save history to file. Embedded newlines are encoded as the two-character
// sequence \n.
CEKSCHEME_API void readline_win_write_history_file(const std::string& path);

// Return a copy of the current history list.
CEKSCHEME_API std::vector<std::string> readline_win_get_history();

// Replace the history list.
CEKSCHEME_API void readline_win_set_history(const std::vector<std::string>& entries);

#endif // _WIN32
