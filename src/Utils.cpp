// Utils.cpp -- Listener-side utility helpers.
// Direct port of pyscheme/Utils.py.
#include "Utils.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <ostream>
#include <string>
#include <vector>

// ── retrieveFileList ──────────────────────────────────────────────────────────

std::vector<std::string> retrieveFileList(const std::string& dirname)
   {
   std::vector<std::string> names;
   for (const auto& entry : std::filesystem::directory_iterator(dirname))
      {
      if (entry.is_regular_file() && entry.path().extension() == ".log")
         names.push_back(entry.path().string());
      }
   std::sort(names.begin(), names.end());
   return names;
   }

// ── columnize helpers ─────────────────────────────────────────────────────────
// Port of Utils.py _Cell, _try_column_layout, _resolve_colors.

struct Cell
   {
   std::string content;
   std::string color; // ANSI escape code or ""
   };

static std::vector<int> try_column_layout(const std::vector<std::string>& lst,
                                          int nrows, int display_width)
   {
   int size = static_cast<int>(lst.size());
   int ncols = (size + nrows - 1) / nrows;
   std::vector<int> colwidths;
   int totwidth = -2;
   for (int col = 0; col < ncols; ++col)
      {
      int colwidth = 0;
      for (int row = 0; row < nrows; ++row)
         {
         int i = row + nrows * col;
         if (i >= size)
            break;
         colwidth = std::max(colwidth, static_cast<int>(lst[i].size()));
         }
      colwidths.push_back(colwidth);
      totwidth += colwidth + 2;
      if (totwidth > display_width)
         return {}; // doesn't fit
      }
   return colwidths;
   }

static std::vector<std::string> resolve_colors(int size,
                                               const std::string& item_color,
                                               const std::vector<std::string>* item_colors)
   {
   if (item_colors)
      return *item_colors;
   return std::vector<std::string>(size, item_color);
   }

// ── columnize ─────────────────────────────────────────────────────────────────

void columnize(const std::vector<std::string>& lst,
               int display_width,
               std::ostream* out,
               const std::string& item_color,
               const std::vector<std::string>* item_colors)
   {
   const std::string RESET = "\033[0m";
   std::ostream& os = out ? *out : std::cout;
   int size = static_cast<int>(lst.size());
   if (size == 0)
      return;
   std::vector<std::string> colors = resolve_colors(size, item_color, item_colors);
   if (size == 1)
      {
      if (!colors[0].empty())
         os << colors[0] << lst[0] << RESET << '\n';
      else
         os << lst[0] << '\n';
      return;
      }

   // Find the smallest nrows for which the layout fits.
   int best_nrows = size;
   int best_ncols = 1;
   std::vector<int> best_colwidths = {0};
   for (int nrows = 1; nrows < size; ++nrows)
      {
      std::vector<int> cw = try_column_layout(lst, nrows, display_width);
      if (!cw.empty())
         {
         best_nrows = nrows;
         best_ncols = static_cast<int>(cw.size());
         best_colwidths = cw;
         break;
         }
      }

   for (int row = 0; row < best_nrows; ++row)
      {
      std::vector<Cell> cells;
      for (int col = 0; col < best_ncols; ++col)
         {
         int i = row + best_nrows * col;
         if (i < size)
            cells.push_back({lst[i], colors[i]});
         else
            cells.push_back({"", ""});
         }
      // Trim trailing empty cells.
      while (!cells.empty() && cells.back().content.empty())
         cells.pop_back();

      for (int col = 0; col < static_cast<int>(cells.size()); ++col)
         {
         if (col > 0)
            os << "  ";
         const std::string& content = cells[col].content;
         const std::string& c = cells[col].color;
         int padlen = best_colwidths[col] - static_cast<int>(content.size());
         std::string padding(padlen < 0 ? 0 : padlen, ' ');
         if (!c.empty() && !content.empty())
            os << c << content << RESET << padding;
         else
            os << content << padding;
         }
      os << '\n';
      }
   }

// ── paren_state ───────────────────────────────────────────────────────────────

ParenState paren_state(const std::string& text)
   {
   std::vector<char> stack;
   bool in_string = false;
   bool in_pipe = false;
   bool escape = false;
   int n = static_cast<int>(text.size());
   int i = 0;
   while (i < n)
      {
      char ch = text[i];
      if (escape)
         {
         escape = false;
         }
      else if (in_string)
         {
         if (ch == '\\')
            escape = true;
         else if (ch == '"')
            in_string = false;
         }
      else if (in_pipe)
         {
         if (ch == '\\')
            escape = true;
         else if (ch == '|')
            in_pipe = false;
         }
      else
         {
         if (ch == '"')
            {
            in_string = true;
            }
         else if (ch == '|')
            {
            in_pipe = true;
            }
         else if (ch == ';')
            {
            while (i < n && text[i] != '\n')
               ++i;
            continue;
            }
         else if (ch == '(' || ch == '[')
            {
            stack.push_back(ch);
            }
         else if (ch == ')' || ch == ']')
            {
            if (!stack.empty())
               stack.pop_back();
            }
         }
      ++i;
      }
   return {static_cast<int>(stack.size()), in_string, stack};
   }

// ── writeln_multiFile ─────────────────────────────────────────────────────────

void writeln_multiFile(const std::string& output_string,
                       const std::vector<std::ostream*>& file_list,
                       bool flush)
   {
   for (std::ostream* f : file_list)
      {
      std::ostream& os = f ? *f : std::cout;
      os << output_string << '\n';
      if (flush)
         os.flush();
      }
   }

// ── Session-log parsing + match semantics ──────────────────────────────────────
// Independent of Listener::parse_log / sessionLog_test (see Utils.h); kept
// behaviourally identical to them.

// Right-strip trailing whitespace (mirror of Listener.cpp's file-local _rstrip).
static std::string _u_rstrip(const std::string& s)
   {
   size_t end = s.size();
   while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                      s[end - 1] == '\r' || s[end - 1] == '\n'))
      --end;
   return s.substr(0, end);
   }

// True if s starts with pfx (length plen).
static bool _u_sw(const std::string& s, const char* pfx, size_t plen)
   {
   return s.size() >= plen && s.compare(0, plen, pfx, plen) == 0;
   }

// Strip leading+trailing whitespace.
static std::string _u_strip(const std::string& s)
   {
   size_t s0 = s.find_first_not_of(" \t\r\n");
   if (s0 == std::string::npos)
      return {};
   size_t s1 = s.find_last_not_of(" \t\r\n");
   return s.substr(s0, s1 - s0 + 1);
   }

std::vector<LogEntry> parse_log(const std::string& text)
   {
   // Split text into lines keeping line endings (splitlines(keepends=True)).
   std::vector<std::string> lines;
   std::string cur;
   for (char c : text)
      {
      cur += c;
      if (c == '\n')
         {
         lines.push_back(cur);
         cur.clear();
         }
      }
   if (!cur.empty())
      lines.push_back(cur);

   auto rstrip_eq = [](const std::string& s, const char* expected, size_t elen) -> bool
   {
      size_t end = s.size();
      while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                         s[end - 1] == '\r' || s[end - 1] == '\n'))
         --end;
      return end == elen && s.compare(0, end, expected, elen) == 0;
   };

   std::vector<LogEntry> entries;
   int idx = 0;
   int n = (int)lines.size();
   bool fold_case = false;

   while (idx < n)
      {
      while (idx < n && !_u_sw(lines[idx], ">>> ", 4))
         {
         if (rstrip_eq(lines[idx], "#!fold-case", 11))
            fold_case = true;
         else if (rstrip_eq(lines[idx], "#!no-fold-case", 14))
            fold_case = false;
         ++idx;
         }
      if (idx >= n)
         break;

      LogEntry entry;
      entry.fold_case = fold_case;
      entry.expr = lines[idx].substr(4);
      ++idx;

      while (idx < n && _u_sw(lines[idx], "... ", 4))
         {
         entry.expr += lines[idx].substr(4);
         ++idx;
         }

      // Optional bare '...' line (old-style multi-line marker).
      if (idx < n && rstrip_eq(lines[idx], "...", 3) && !_u_sw(lines[idx], "... ", 4))
         ++idx;

      while (idx < n)
         {
         const std::string& line = lines[idx];
         if (_u_sw(line, "==> ", 4) || rstrip_eq(line, "==>", 3))
            break;
         if (_u_sw(line, "... ", 4) || _u_sw(line, ">>> ", 4) || _u_sw(line, "%%% ", 4))
            break;
         entry.output += line;
         ++idx;
         }

      if (idx < n && (_u_sw(lines[idx], "==> ", 4) || rstrip_eq(lines[idx], "==>", 3)))
         {
         const std::string& line = lines[idx];
         if (line.size() > 4)
            entry.retval = line.substr(4);
         ++idx;
         while (idx < n)
            {
            const std::string& ln = lines[idx];
            if (_u_sw(ln, "==> ", 4) || rstrip_eq(ln, "==>", 3))
               break;
            if (_u_sw(ln, "... ", 4) || _u_sw(ln, ">>> ", 4) || _u_sw(ln, "%%% ", 4))
               break;
            if (_u_sw(ln, "#!", 2))
               break; // fold-case directive
            if (!ln.empty() && ln[0] == ';')
               entry.expr += ln;
            else
               entry.retval += ln;
            ++idx;
            }
         }

      if (idx < n && _u_sw(lines[idx], "%%% ", 4))
         {
         entry.error = lines[idx].substr(4);
         ++idx;
         while (idx < n && _u_sw(lines[idx], "%%% ", 4))
            {
            entry.error += lines[idx].substr(4);
            ++idx;
            }
         }

      if (!entry.expr.empty())
         {
         entry.output = _u_rstrip(entry.output);
         entry.retval = _u_rstrip(entry.retval);
         entry.error = _u_rstrip(entry.error);
         entries.push_back(std::move(entry));
         }
      }
   return entries;
   }

bool log_match_retval(const std::string& actual, const std::string& expected)
   {
   const std::string sep = " or ==> ";
   std::string rem = expected;
   while (true)
      {
      size_t idx = rem.find(sep);
      std::string part = (idx == std::string::npos) ? rem : rem.substr(0, idx);
      part = _u_strip(part);
      if (actual == part)
         return true;
      if (idx == std::string::npos)
         break;
      rem = rem.substr(idx + sep.size());
      }
   return false;
   }

LogMatch log_match(const std::string& expected_output,
                   const std::string& expected_retval,
                   const std::string& expected_error,
                   const std::string& actual_output,
                   const std::string& actual_retval,
                   const std::string& actual_error,
                   bool timed_out)
   {
   std::string exp_out = _u_rstrip(expected_output);
   std::string act_out = _u_rstrip(actual_output);
   if (timed_out)
      return {false, false, false};
   if (_u_sw(expected_error, "%optional-error%", 16))
      return {true, true, true};
   bool error_ok;
   if (expected_error == "*" || _u_sw(expected_error, "%any-error%", 11))
      error_ok = !actual_error.empty();
   else
      error_ok = (actual_error == expected_error);
   bool retval_ok = log_match_retval(actual_retval, expected_retval);
   bool output_ok = (act_out == exp_out);
   return {output_ok, retval_ok, error_ok};
   }
