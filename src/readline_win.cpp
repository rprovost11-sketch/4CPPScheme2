// readline_win.cpp -- Windows console readline replacement using _getwch().
// Direct port of pyscheme/readline_win.py.
#include "readline_win.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <conio.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// ── Module state ───────────────────────────────────────────────────────────────
static std::vector<std::string> s_history;
static int s_history_max = 500;

// ── UTF-8 / wide-char conversion ───────────────────────────────────────────────
static std::string wvec_to_utf8(const std::vector<wchar_t>& v)
   {
   if (v.empty())
      return {};
   int len = WideCharToMultiByte(CP_UTF8, 0, v.data(), (int)v.size(),
                                 nullptr, 0, nullptr, nullptr);
   if (len <= 0)
      return {};
   std::string s(len, '\0');
   WideCharToMultiByte(CP_UTF8, 0, v.data(), (int)v.size(),
                       s.data(), len, nullptr, nullptr);
   return s;
   }

static std::vector<wchar_t> utf8_to_wvec(const std::string& s)
   {
   if (s.empty())
      return {};
   int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
   if (len <= 0)
      return {};
   std::vector<wchar_t> v(len);
   MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), v.data(), len);
   return v;
   }

// ── Key decoding ───────────────────────────────────────────────────────────────
enum KeyCode
   {
   KEY_ENTER,
   KEY_BACKSPACE,
   KEY_DELETE,
   KEY_LEFT,
   KEY_RIGHT,
   KEY_UP,
   KEY_DOWN,
   KEY_HOME,
   KEY_END,
   KEY_CTRL_C,
   KEY_CTRL_R,
   KEY_EOF,
   KEY_ESCAPE,
   KEY_PRINTABLE,
   KEY_UNKNOWN,
   };

struct Key
   {
   KeyCode code;
   wchar_t ch;
   };

// Second-byte map for \xe0-prefixed extended key sequences.
static Key decode_extended(wchar_t ch2)
   {
   switch (ch2)
      {
   case L'\x48':
      return {KEY_UP, 0};
   case L'\x50':
      return {KEY_DOWN, 0};
   case L'\x4b':
      return {KEY_LEFT, 0};
   case L'\x4d':
      return {KEY_RIGHT, 0};
   case L'\x47':
      return {KEY_HOME, 0};
   case L'\x4f':
      return {KEY_END, 0};
   case L'\x53':
      return {KEY_DELETE, 0};
   default:
      return {KEY_UNKNOWN, 0};
      }
   }

static Key read_key()
   {
   wchar_t ch = _getwch();
   if (ch == L'\x00' || ch == L'\xe0')
      return decode_extended(_getwch());
   if (ch == L'\r')
      return {KEY_ENTER, 0};
   if (ch == L'\x08')
      return {KEY_BACKSPACE, 0};
   if (ch == L'\x03')
      return {KEY_CTRL_C, 0};
   if (ch == L'\x12')
      return {KEY_CTRL_R, 0};
   if (ch == L'\x1b')
      return {KEY_ESCAPE, 0};
   if (ch == L'\x04' || ch == L'\x1a')
      return {KEY_EOF, 0};
   if (iswprint(ch))
      return {KEY_PRINTABLE, ch};
   return {KEY_UNKNOWN, 0};
   }

// ── Display helpers ────────────────────────────────────────────────────────────

// Split the wchar_t buffer into UTF-8 lines on L'\n' boundaries.
static std::vector<std::string> split_wbuf(const std::vector<wchar_t>& buf)
   {
   std::vector<std::string> result;
   std::vector<wchar_t> cur;
   for (wchar_t c : buf)
      {
      if (c == L'\n')
         {
         result.push_back(wvec_to_utf8(cur));
         cur.clear();
         }
      else
         cur.push_back(c);
      }
   result.push_back(wvec_to_utf8(cur));
   return result;
   }

// Redraw the prompt and buffer; position the cursor.  Updates prev_extra.
static void do_redraw(const std::vector<wchar_t>& buf, int cursor,
                      const std::string& prompt,
                      const std::string& continuation_prompt,
                      int& prev_extra)
   {
   auto parts = split_wbuf(buf);
   int extra = (int)parts.size() - 1;

   // Move up to the first line if the previous redraw drew multiple lines.
   if (prev_extra > 0)
      std::cout << "\033[" << prev_extra << "A";

   // Draw each line with the appropriate prompt.
   for (int i = 0; i < (int)parts.size(); ++i)
      {
      const std::string& pfx = (i == 0) ? prompt : continuation_prompt;
      if (i > 0)
         std::cout << '\n';
      std::cout << '\r' << pfx << parts[i] << "\033[K";
      }

   // Clear any lines left over from a previous longer draw.
   int leftover = prev_extra - extra;
   if (leftover > 0)
      {
      for (int k = 0; k < leftover; ++k)
         std::cout << "\n\r\033[K";
      std::cout << "\033[" << leftover << "A";
      }

   // Compute cursor's display line and column.
   int cursor_line = 0, cursor_col = 0;
   for (int k = 0; k < cursor; ++k)
      {
      if (buf[k] == L'\n')
         {
         ++cursor_line;
         cursor_col = 0;
         }
      else
         ++cursor_col;
      }
   const std::string& cursor_pfx = (cursor_line == 0) ? prompt : continuation_prompt;

   int lines_up = extra - cursor_line;
   if (lines_up > 0)
      std::cout << "\033[" << lines_up << "A";

   int target_col = (int)cursor_pfx.size() + cursor_col;
   std::cout << '\r';
   if (target_col > 0)
      std::cout << "\033[" << target_col << "C";

   std::cout.flush();
   prev_extra = extra;
   }

// Position cursor at end of last display line then emit a newline.
static void do_move_end_newline(const std::vector<wchar_t>& buf, int cursor)
   {
   int extra = 0;
   for (wchar_t c : buf)
      if (c == L'\n')
         ++extra;
   int cursor_line = 0;
   for (int k = 0; k < cursor; ++k)
      if (buf[k] == L'\n')
         ++cursor_line;
   int lines_down = extra - cursor_line;
   if (lines_down > 0)
      std::cout << "\033[" << lines_down << "B";
   std::cout << '\n';
   std::cout.flush();
   }

// ── Public API ─────────────────────────────────────────────────────────────────

std::string readline_win_input_line(const std::string& prompt,
                                    const std::string& continuation_prompt,
                                    const std::string& prefill)
   {
   // Enable ANSI escape code processing and UTF-8 output on first call.
   static bool s_ansi_enabled = false;
   if (!s_ansi_enabled)
      {
      HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
      DWORD m = 0;
      if (GetConsoleMode(h, &m))
         SetConsoleMode(h, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
      SetConsoleOutputCP(CP_UTF8);
      s_ansi_enabled = true;
      }

   std::vector<wchar_t> buf = utf8_to_wvec(prefill);
   int cursor = (int)buf.size();
   int hist_idx = -1;             // -1 = editing current; 0 = newest entry
   std::string hist_pending_utf8; // saves in-progress line while navigating
   int prev_extra = 0;            // extra display lines from previous redraw
   Key pending_key = {KEY_UNKNOWN, 0};
   bool has_pending = false;

   do_redraw(buf, cursor, prompt, continuation_prompt, prev_extra);

   while (true)
      {
      Key key;
      if (has_pending)
         {
         key = pending_key;
         has_pending = false;
         }
      else
         key = read_key();

      switch (key.code)
         {
      case KEY_UNKNOWN:
         break;

      case KEY_ENTER:
         do_move_end_newline(buf, cursor);
         return wvec_to_utf8(buf);

      case KEY_CTRL_C:
         do_move_end_newline(buf, cursor);
         throw ReadlineInterruptError();

      case KEY_EOF:
         if (buf.empty())
            {
            do_move_end_newline(buf, cursor);
            throw ReadlineEOFError();
            }
         // Ctrl-D with content -- ignore (matches GNU readline behaviour).
         break;

      case KEY_BACKSPACE:
         if (cursor > 0)
            {
            buf.erase(buf.begin() + cursor - 1);
            --cursor;
            do_redraw(buf, cursor, prompt, continuation_prompt, prev_extra);
            }
         break;

      case KEY_DELETE:
         if (cursor < (int)buf.size())
            {
            buf.erase(buf.begin() + cursor);
            do_redraw(buf, cursor, prompt, continuation_prompt, prev_extra);
            }
         break;

      case KEY_LEFT:
         if (cursor > 0)
            {
            --cursor;
            do_redraw(buf, cursor, prompt, continuation_prompt, prev_extra);
            }
         break;

      case KEY_RIGHT:
         if (cursor < (int)buf.size())
            {
            ++cursor;
            do_redraw(buf, cursor, prompt, continuation_prompt, prev_extra);
            }
         break;

      case KEY_HOME:
         cursor = 0;
         do_redraw(buf, cursor, prompt, continuation_prompt, prev_extra);
         break;

      case KEY_END:
         cursor = (int)buf.size();
         do_redraw(buf, cursor, prompt, continuation_prompt, prev_extra);
         break;

      case KEY_UP:
         if (hist_idx == -1)
            hist_pending_utf8 = wvec_to_utf8(buf);
            {
            int next_idx = hist_idx + 1;
            if (next_idx < (int)s_history.size())
               {
               hist_idx = next_idx;
               buf = utf8_to_wvec(s_history[(int)s_history.size() - 1 - hist_idx]);
               cursor = (int)buf.size();
               do_redraw(buf, cursor, prompt, continuation_prompt, prev_extra);
               }
            }
         break;

      case KEY_DOWN:
         if (hist_idx > 0)
            {
            --hist_idx;
            buf = utf8_to_wvec(s_history[(int)s_history.size() - 1 - hist_idx]);
            cursor = (int)buf.size();
            do_redraw(buf, cursor, prompt, continuation_prompt, prev_extra);
            }
         else if (hist_idx == 0)
            {
            hist_idx = -1;
            buf = utf8_to_wvec(hist_pending_utf8);
            cursor = (int)buf.size();
            do_redraw(buf, cursor, prompt, continuation_prompt, prev_extra);
            }
         break;

      case KEY_CTRL_R:
         {
         // Reverse incremental history search.
         std::string search_str;
         int found_idx = -1;
         std::vector<wchar_t> found_buf;
         std::vector<wchar_t> saved_buf = buf;
         int saved_curs = cursor;

         auto do_hist_search = [&](int from)
         {
            for (int i = from; i >= 0; --i)
               {
               if (s_history[i].find(search_str) != std::string::npos)
                  {
                  found_idx = i;
                  found_buf = utf8_to_wvec(s_history[i]);
                  return;
                  }
               }
            found_idx = -1;
            found_buf.clear();
         };

         auto search_draw = [&]()
         {
            std::string first;
            if (found_idx >= 0)
               {
               std::string entry = wvec_to_utf8(found_buf);
               size_t nl = entry.find('\n');
               first = (nl != std::string::npos) ? entry.substr(0, nl) : entry;
               }
            std::string spfx = "(reverse-i-search)'" + search_str + "': ";
            if (prev_extra > 0)
               {
               std::cout << "\033[" << prev_extra << "A";
               prev_extra = 0;
               }
            std::cout << '\r' << spfx << first << "\033[K";
            std::cout.flush();
         };

         search_draw();

         while (true)
            {
            Key skey = read_key();

            if (skey.code == KEY_CTRL_R)
               {
               if (!search_str.empty())
                  {
                  if (found_idx > 0)
                     do_hist_search(found_idx - 1);
                  else if (found_idx == -1)
                     do_hist_search((int)s_history.size() - 1);
                  // found_idx == 0: already at oldest, don't search further
                  }
               search_draw();
               }
            else if (skey.code == KEY_BACKSPACE)
               {
               if (!search_str.empty())
                  {
                  search_str.pop_back();
                  if (!search_str.empty())
                     do_hist_search((int)s_history.size() - 1);
                  else
                     {
                     found_idx = -1;
                     found_buf.clear();
                     }
                  }
               search_draw();
               }
            else if (skey.code == KEY_ENTER)
               {
               buf = (found_idx >= 0) ? found_buf : saved_buf;
               cursor = (int)buf.size();
               prev_extra = 0;
               std::cout << '\n';
               std::cout.flush();
               return wvec_to_utf8(buf);
               }
            else if (skey.code == KEY_CTRL_C)
               {
               buf = saved_buf;
               cursor = saved_curs;
               prev_extra = 0;
               do_move_end_newline(buf, cursor);
               throw ReadlineInterruptError();
               }
            else if (skey.code == KEY_ESCAPE || skey.code == KEY_UNKNOWN)
               {
               buf = saved_buf;
               cursor = saved_curs;
               prev_extra = 0;
               do_redraw(buf, cursor, prompt, continuation_prompt, prev_extra);
               break;
               }
            else if (skey.code == KEY_PRINTABLE)
               {
               std::vector<wchar_t> tmp = {skey.ch};
               search_str += wvec_to_utf8(tmp);
               do_hist_search((int)s_history.size() - 1);
               search_draw();
               }
            else
               {
               // Any other key: accept match, requeue key for normal handling.
               buf = (found_idx >= 0) ? found_buf : saved_buf;
               cursor = (int)buf.size();
               prev_extra = 0;
               pending_key = skey;
               has_pending = true;
               do_redraw(buf, cursor, prompt, continuation_prompt, prev_extra);
               break;
               }
            }
         break;
         }

      case KEY_PRINTABLE:
         buf.insert(buf.begin() + cursor, key.ch);
         ++cursor;
         do_redraw(buf, cursor, prompt, continuation_prompt, prev_extra);
         break;

      default:
         break;
         }
      }
   }

void readline_win_add_history(const std::string& entry)
   {
   if (entry.empty())
      return;
   if (!s_history.empty() && s_history.back() == entry)
      return;
   s_history.push_back(entry);
   if ((int)s_history.size() > s_history_max)
      s_history.erase(s_history.begin());
   }

void readline_win_set_history_length(int n)
   {
   s_history_max = n;
   }

void readline_win_read_history_file(const std::string& path)
   {
   std::ifstream f(path, std::ios::in);
   if (!f)
      return;
   std::string raw;
   while (std::getline(f, raw))
      {
      std::string entry;
      for (size_t i = 0; i < raw.size(); ++i)
         {
         if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == 'n')
            {
            entry += '\n';
            ++i;
            }
         else
            {
            entry += raw[i];
            }
         }
      if (!entry.empty())
         s_history.push_back(entry);
      }
   }

void readline_win_write_history_file(const std::string& path)
   {
   int n = (int)s_history.size();
   int start = std::max(0, n - s_history_max);
   std::ofstream f(path, std::ios::out);
   if (!f)
      return;
   for (int i = start; i < n; ++i)
      {
      for (char c : s_history[i])
         {
         if (c == '\n')
            f << "\\n";
         else
            f << c;
         }
      f << '\n';
      }
   }

std::vector<std::string> readline_win_get_history()
   {
   return s_history;
   }
void readline_win_set_history(const std::vector<std::string>& entries)
   {
   s_history = entries;
   }

#endif // _WIN32
