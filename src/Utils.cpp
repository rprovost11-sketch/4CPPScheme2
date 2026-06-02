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
