// primitives/strings.cpp -- string primitives.
// Direct port of pyscheme/primitives/strings.py.
//
// Strings are stored as std::string (UTF-8 bytes).  All indexing and length
// operations work on Unicode code points (characters), matching PyScheme's
// char-per-code-point semantics via Python str.
#include "strings.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"
#include "../Evaluator.h"
#include "../gc.h"
#include "../unicode_tables.h"
#include <algorithm>
#include <cctype>

static const char* CATEGORY = "strings";

static SourceInfo* _src(const Value* a)
   {
   return a ? src_of(*a) : nullptr;
   }

// ── UTF-8 helpers ─────────────────────────────────────────────────────────────

// Advance pos past one UTF-8 code point; return the code point.
// Declared in AST.h (shared with the evaluator's HOF frames).
char32_t utf8_next(const std::string& s, size_t& pos)
   {
   auto b0 = static_cast<unsigned char>(s[pos]);
   if (b0 < 0x80)
      {
      pos += 1;
      return char32_t(b0);
      }
   if ((b0 & 0xE0) == 0xC0 && pos + 1 < s.size())
      {
      char32_t c = ((b0 & 0x1F) << 6) | (static_cast<unsigned char>(s[pos + 1]) & 0x3F);
      pos += 2;
      return c;
      }
   if ((b0 & 0xF0) == 0xE0 && pos + 2 < s.size())
      {
      char32_t c = ((b0 & 0x0F) << 12) | ((static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 6) | (static_cast<unsigned char>(s[pos + 2]) & 0x3F);
      pos += 3;
      return c;
      }
   if ((b0 & 0xF8) == 0xF0 && pos + 3 < s.size())
      {
      char32_t c = ((b0 & 0x07) << 18) | ((static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 12) | ((static_cast<unsigned char>(s[pos + 2]) & 0x3F) << 6) | (static_cast<unsigned char>(s[pos + 3]) & 0x3F);
      pos += 4;
      return c;
      }
   pos += 1;
   return char32_t(b0); // invalid UTF-8: treat as Latin-1
   }

// Count Unicode code points in s.
static int64_t utf8_char_count(const std::string& s)
   {
   int64_t n = 0;
   for (size_t i = 0; i < s.size();)
      {
      utf8_next(s, i);
      ++n;
      }
   return n;
   }

// Return the byte offset of character position k in s.
// Assumes 0 <= k <= utf8_char_count(s); returns s.size() when k == char_count.
static size_t utf8_char_offset(const std::string& s, int64_t k)
   {
   size_t pos = 0;
   for (int64_t i = 0; i < k && pos < s.size(); ++i)
      utf8_next(s, pos);
   return pos;
   }

// Encode one code point as UTF-8 into result.
void utf8_encode(std::string& out, char32_t c)
   {
   if (c < 0x80)
      {
      out += static_cast<char>(c);
      }
   else if (c < 0x800)
      {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
      }
   else if (c < 0x10000)
      {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
      }
   else
      {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
      }
   }

static const std::string& _check_string(const Value& v, const char* name, const Value* app, int idx = 1)
   {
   if (!is_string(v))
      throw SchemeTypeError(
          std::string(name) + ": argument " + std::to_string(idx) + " must be a string",
          _src(app));
   return as_string(v);
   }

static Value _prim_string_length(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   return make_integer(utf8_char_count(_check_string(args[0], "string-length", app)));
   }

static Value _prim_string_ref(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const std::string& s = _check_string(args[0], "string-ref", app);
   int64_t nchars = utf8_char_count(s);
   int64_t k = check_index(args[1], "string-ref", nchars, app);
   size_t pos = utf8_char_offset(s, k);
   return make_character(utf8_next(s, pos));
   }

static Value _string_compare(const char* name, std::vector<Value>& args, const Value* app,
                             bool (*op)(const std::string&, const std::string&))
   {
   if (args.size() < 2)
      return make_boolean(true);
   const std::string* prev = &_check_string(args[0], name, app, 1);
   for (size_t i = 1; i < args.size(); ++i)
      {
      const std::string& cur = _check_string(args[i], name, app, static_cast<int>(i + 1));
      if (!op(*prev, cur))
         return make_boolean(false);
      prev = &cur;
      }
   return make_boolean(true);
   }

static Value _prim_string_eq(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _string_compare("string=?", a, n, [](const std::string& x, const std::string& y)
                          { return x == y; });
   }
static Value _prim_string_lt(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _string_compare("string<?", a, n, [](const std::string& x, const std::string& y)
                          { return x < y; });
   }
static Value _prim_string_le(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _string_compare("string<=?", a, n, [](const std::string& x, const std::string& y)
                          { return x <= y; });
   }
static Value _prim_string_gt(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _string_compare("string>?", a, n, [](const std::string& x, const std::string& y)
                          { return x > y; });
   }
static Value _prim_string_ge(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _string_compare("string>=?", a, n, [](const std::string& x, const std::string& y)
                          { return x >= y; });
   }

static Value _prim_substring(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const std::string& s = _check_string(args[0], "substring", app);
   int64_t nchars = utf8_char_count(s);
   auto [start, end] = parse_start_end(args, 1, nchars, "substring", app);
   size_t bstart = utf8_char_offset(s, start);
   size_t bend = utf8_char_offset(s, end);
   return make_string(s.substr(bstart, bend - bstart));
   }

static Value _prim_string_append(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   std::string result;
   for (size_t i = 0; i < args.size(); ++i)
      result += _check_string(args[i], "string-append", app, static_cast<int>(i + 1));
   return make_string(result);
   }

static Value _prim_string_to_list(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const std::string& s = _check_string(args[0], "string->list", app);
   int64_t nchars = utf8_char_count(s);
   auto [start, end] = parse_start_end(args, 1, nchars, "string->list", app);
   // Collect chars [start, end) then reverse-cons into a list.
   std::vector<char32_t> chars;
   size_t pos = utf8_char_offset(s, start);
   for (int64_t i = start; i < end; ++i)
      chars.push_back(utf8_next(s, pos));
   Value result = NIL_VALUE;
   for (int64_t i = (int64_t)chars.size() - 1; i >= 0; --i)
      result = alloc_cons(make_character(chars[(size_t)i]), result);
   return result;
   }

static Value _prim_list_to_string(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   std::string result;
   Value cur = args[0];
   while (is_cons(cur))
      {
      Value c = car(cur);
      if (!is_character(c))
         throw SchemeTypeError("list->string: list elements must be characters", _src(app));
      utf8_encode(result, as_character(c));
      cur = cdr(cur);
      }
   if (!is_nil(cur))
      throw SchemeTypeError("list->string: argument must be a proper list", _src(app));
   return make_string(result);
   }

static Value _prim_string_copy(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const std::string& s = _check_string(args[0], "string-copy", app);
   int64_t nchars = utf8_char_count(s);
   auto [start, end] = parse_start_end(args, 1, nchars, "string-copy", app);
   size_t bstart = utf8_char_offset(s, start);
   size_t bend = utf8_char_offset(s, end);
   return make_string(s.substr(bstart, bend - bstart));
   }

static Value _prim_make_string(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_integer(args[0]))
      throw SchemeTypeError("make-string: length must be an integer", _src(app));
   int64_t k = as_integer(args[0]);
   if (k < 0)
      throw SchemeTypeError("make-string: length must be non-negative", _src(app));
   char32_t fill = ' ';
   if (args.size() >= 2)
      {
      if (!is_character(args[1]))
         throw SchemeTypeError("make-string: fill must be a character", _src(app));
      fill = as_character(args[1]);
      }
   std::string unit;
   utf8_encode(unit, fill);
   std::string result;
   for (int64_t i = 0; i < k; ++i)
      result += unit;
   return make_string(result);
   }

static Value _prim_string(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   std::string result;
   for (size_t i = 0; i < args.size(); ++i)
      {
      if (!is_character(args[i]))
         throw SchemeTypeError(
             "string: argument " + std::to_string(i + 1) + " must be a character", _src(app));
      utf8_encode(result, as_character(args[i]));
      }
   return make_string(result);
   }

static Value _prim_string_to_symbol(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   return make_symbol(_check_string(args[0], "string->symbol", app));
   }

static Value _prim_symbol_to_string(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_symbol(args[0]))
      throw SchemeTypeError("symbol->string: argument must be a symbol", _src(app));
   return make_string(as_symbol(args[0]));
   }

// Map a UTF-8 string codepoint-by-codepoint through a full Unicode case
// mapping (1:N), re-encoding to UTF-8.  Per-codepoint -- no context rules (see
// unicode_tables.h note on Greek final sigma).
static std::string _string_case(const std::string& s,
                                int (*mapfn)(char32_t, char32_t*))
   {
   std::string out;
   out.reserve(s.size());
   size_t pos = 0;
   while (pos < s.size())
      {
      char32_t cp = utf8_next(s, pos);
      char32_t mapped[3];
      int n = mapfn(cp, mapped);
      for (int i = 0; i < n; ++i)
         utf8_encode(out, mapped[i]);
      }
   return out;
   }

static Value _prim_string_upcase(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   return make_string(_string_case(_check_string(args[0], "string-upcase", app),
                                   unicode::upcase));
   }

static Value _prim_string_downcase(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   return make_string(_string_case(_check_string(args[0], "string-downcase", app),
                                   unicode::downcase));
   }

static Value _prim_string_foldcase(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   return make_string(_string_case(_check_string(args[0], "string-foldcase", app),
                                   unicode::foldcase));
   }

// Case-folding for string-ci* (R7RS uses full case folding, matching
// pyscheme's str.casefold()).
static std::string _to_lower(const std::string& s)
   {
   return _string_case(s, unicode::foldcase);
   }

static Value _string_compare_ci(const char* name, std::vector<Value>& args, const Value* app,
                                bool (*op)(const std::string&, const std::string&))
   {
   if (args.size() < 2)
      return make_boolean(true);
   std::string prev = _to_lower(_check_string(args[0], name, app, 1));
   for (size_t i = 1; i < args.size(); ++i)
      {
      std::string cur = _to_lower(_check_string(args[i], name, app, static_cast<int>(i + 1)));
      if (!op(prev, cur))
         return make_boolean(false);
      prev = cur;
      }
   return make_boolean(true);
   }

static Value _prim_string_ci_eq(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _string_compare_ci("string-ci=?", a, n, [](const std::string& x, const std::string& y)
                             { return x == y; });
   }
static Value _prim_string_ci_lt(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _string_compare_ci("string-ci<?", a, n, [](const std::string& x, const std::string& y)
                             { return x < y; });
   }
static Value _prim_string_ci_le(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _string_compare_ci("string-ci<=?", a, n, [](const std::string& x, const std::string& y)
                             { return x <= y; });
   }
static Value _prim_string_ci_gt(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _string_compare_ci("string-ci>?", a, n, [](const std::string& x, const std::string& y)
                             { return x > y; });
   }
static Value _prim_string_ci_ge(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _string_compare_ci("string-ci>=?", a, n, [](const std::string& x, const std::string& y)
                             { return x >= y; });
   }

static Value _prim_string_map(Context* ctx, Environment* env, std::vector<Value>& args, const Value* app)
   {
   if (args.size() < 2)
      throw SchemeTypeError("string-map: at least one string is required", _src(app));
   GcRootVec args_root(args); // keep SchemeString objects alive across apply_scheme_proc calls
   GcRootGuard proc(args[0]);
   std::vector<const std::string*> strs;
   for (size_t i = 1; i < args.size(); ++i)
      strs.push_back(&_check_string(args[i], "string-map", app, static_cast<int>(i + 1)));
   // Walk all strings in parallel by character (shortest wins).
   std::vector<size_t> positions(strs.size(), 0);
   std::string result;
   for (;;)
      {
      bool any_done = false;
      for (size_t si = 0; si < strs.size(); ++si)
         if (positions[si] >= strs[si]->size())
            {
            any_done = true;
            break;
            }
      if (any_done)
         break;
      std::vector<Value> row;
      GcRootVec row_root(row);
      for (size_t si = 0; si < strs.size(); ++si)
         row.push_back(make_character(utf8_next(*strs[si], positions[si])));
      Value ch_val = apply_scheme_proc(proc.val, std::move(row), ctx, env, app);
      if (!is_character(ch_val))
         throw SchemeTypeError("string-map: proc must return a character", _src(app));
      utf8_encode(result, as_character(ch_val));
      }
   return make_string(result);
   }

static Value _prim_string_for_each(Context* ctx, Environment* env, std::vector<Value>& args, const Value* app)
   {
   if (args.size() < 2)
      throw SchemeTypeError("string-for-each: at least one string is required", _src(app));
   GcRootVec args_root(args); // keep SchemeString objects alive across apply_scheme_proc calls
   GcRootGuard proc(args[0]);
   std::vector<const std::string*> strs;
   for (size_t i = 1; i < args.size(); ++i)
      strs.push_back(&_check_string(args[i], "string-for-each", app, static_cast<int>(i + 1)));
   std::vector<size_t> positions(strs.size(), 0);
   for (;;)
      {
      bool any_done = false;
      for (size_t si = 0; si < strs.size(); ++si)
         if (positions[si] >= strs[si]->size())
            {
            any_done = true;
            break;
            }
      if (any_done)
         break;
      std::vector<Value> row;
      GcRootVec row_root(row);
      for (size_t si = 0; si < strs.size(); ++si)
         row.push_back(make_character(utf8_next(*strs[si], positions[si])));
      apply_scheme_proc(proc.val, std::move(row), ctx, env, app);
      }
   return VOID_VALUE;
   }

// string-set!: Port of strings.py _prim_string_set_bang.
// Python mutates s._s directly; C++ uses as_string_mut().
static Value _prim_string_set_bang(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("string-set!: argument 1 must be a string", _src(app));
   if (is_immutable(args[0]))
      throw SchemeTypeError("string-set!: argument is an immutable literal", _src(app));
   if (!is_integer(args[1]))
      throw SchemeTypeError("string-set!: index must be an integer", _src(app));
   if (!is_character(args[2]))
      throw SchemeTypeError("string-set!: third argument must be a character", _src(app));
   std::string& sm = as_string_mut(args[0]);
   int64_t k = as_integer(args[1]);
   if (k < 0 || static_cast<size_t>(k) >= sm.size())
      throw SchemeTypeError(
          "string-set!: index " + std::to_string(k) +
              " out of range for string of length " + std::to_string(sm.size()),
          _src(app));
   char32_t ch = as_character(args[2]);
   sm[static_cast<size_t>(k)] = static_cast<char>(ch & 0x7F); // ASCII assumption
   return VOID_VALUE;
   }

// string-fill!: Port of strings.py _prim_string_fill_bang.
static Value _prim_string_fill_bang(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("string-fill!: argument 1 must be a string", _src(app));
   if (is_immutable(args[0]))
      throw SchemeTypeError("string-fill!: argument is an immutable literal", _src(app));
   if (!is_character(args[1]))
      throw SchemeTypeError("string-fill!: second argument must be a character", _src(app));
   std::string& sm = as_string_mut(args[0]);
   char32_t ch = as_character(args[1]);
   char fill_byte = static_cast<char>(ch & 0x7F);
   size_t n = sm.size();
   auto [start, end] = parse_start_end(args, 2, static_cast<int64_t>(n),
                                       "string-fill!", app, "range out of bounds");
   for (int64_t j = start; j < end; ++j)
      sm[static_cast<size_t>(j)] = fill_byte;
   return VOID_VALUE;
   }

// string-copy!: Port of strings.py _prim_string_copy_bang.
static Value _prim_string_copy_bang(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("string-copy!: argument 1 must be a string", _src(app));
   if (!is_integer(args[1]))
      throw SchemeTypeError("string-copy!: at must be an integer", _src(app));
   if (!is_string(args[2]))
      throw SchemeTypeError("string-copy!: argument 3 must be a string", _src(app));
   std::string& to = as_string_mut(args[0]);
   int64_t at = as_integer(args[1]);
   const std::string& frm = as_string(args[2]);
   auto [start, end] = parse_start_end(args, 3, static_cast<int64_t>(frm.size()),
                                       "string-copy!", app, "source range out of bounds");
   int64_t chunk_len = end - start;
   if (at < 0 || at + chunk_len > static_cast<int64_t>(to.size()))
      throw SchemeTypeError("string-copy!: destination range out of bounds", _src(app));
   // memmove semantics for potential alias
   std::memmove(to.data() + at, frm.data() + start, static_cast<size_t>(chunk_len));
   return VOID_VALUE;
   }

void register_strings()
   {
   register_primitive("string-length", 1, 1, _prim_string_length, "", "Return the number of characters in the string.  R7RS 6.7.", CATEGORY);
   register_primitive("string-ref", 2, 2, _prim_string_ref, "", "(string-ref string k) returns the kth character.  R7RS 6.7.", CATEGORY);
   register_primitive("string=?", 2, -1, _prim_string_eq, "", "Return #t if all string arguments compare equal.  R7RS 6.7.", CATEGORY);
   register_primitive("string<?", 2, -1, _prim_string_lt, "", "Return #t if the strings are in strictly ascending order.  R7RS 6.7.", CATEGORY);
   register_primitive("string<=?", 2, -1, _prim_string_le, "", "Return #t if the strings are in non-descending order.  R7RS 6.7.", CATEGORY);
   register_primitive("string>?", 2, -1, _prim_string_gt, "", "Return #t if the strings are in strictly descending order.  R7RS 6.7.", CATEGORY);
   register_primitive("string>=?", 2, -1, _prim_string_ge, "", "Return #t if the strings are in non-ascending order.  R7RS 6.7.", CATEGORY);
   register_primitive("substring", 2, 3, _prim_substring, "",
                      "(substring string start [end]) returns the substring from index start (inclusive) to end (exclusive).  R7RS 6.7.", CATEGORY);
   register_primitive("string-append", 0, -1, _prim_string_append, "", "Concatenate string arguments into a new string.  R7RS 6.7.", CATEGORY);
   register_primitive("string->list", 1, 3, _prim_string_to_list, "", "(string->list string [start [end]]) returns a list of the characters in string[start:end].  R7RS 6.7.", CATEGORY);
   register_primitive("list->string", 1, 1, _prim_list_to_string, "", "Return a string built from the characters in the list.  R7RS 6.7.", CATEGORY);
   register_primitive("string-copy", 1, 3, _prim_string_copy, "",
                      "(string-copy string [start [end]]) returns a copy of (a slice of) the string.  R7RS 6.7.", CATEGORY);
   register_primitive("make-string", 1, 2, _prim_make_string, "",
                      "(make-string k [char]) returns a string of length k filled with char (default space).  R7RS 6.7.", CATEGORY);
   register_primitive("string", 0, -1, _prim_string, "", "Return a string composed of the character arguments.  R7RS 6.7.", CATEGORY);
   register_primitive("string->symbol", 1, 1, _prim_string_to_symbol, "", "Return a symbol whose name is the string.  R7RS 6.5.", CATEGORY);
   register_primitive("symbol->string", 1, 1, _prim_symbol_to_string, "", "Return the symbol's name as a string.  R7RS 6.5.", CATEGORY);
   register_primitive("string-upcase", 1, 1, _prim_string_upcase, "", "Return the string with each character upcased.  R7RS 6.7.", CATEGORY);
   register_primitive("string-downcase", 1, 1, _prim_string_downcase, "", "Return the string with each character downcased.  R7RS 6.7.", CATEGORY);
   register_primitive("string-foldcase", 1, 1, _prim_string_foldcase, "", "Return the string with Unicode full case folding applied.  R7RS 6.7.", CATEGORY);
   register_primitive("string-ci=?", 2, -1, _prim_string_ci_eq, "", "Case-insensitive string=?.  R7RS 6.7.", CATEGORY);
   register_primitive("string-ci<?", 2, -1, _prim_string_ci_lt, "", "Case-insensitive string<?.  R7RS 6.7.", CATEGORY);
   register_primitive("string-ci<=?", 2, -1, _prim_string_ci_le, "", "Case-insensitive string<=?.  R7RS 6.7.", CATEGORY);
   register_primitive("string-ci>?", 2, -1, _prim_string_ci_gt, "", "Case-insensitive string>?.  R7RS 6.7.", CATEGORY);
   register_primitive("string-ci>=?", 2, -1, _prim_string_ci_ge, "", "Case-insensitive string>=?.  R7RS 6.7.", CATEGORY);
   register_primitive("string-map", 2, -1, _prim_string_map, "",
                      "(string-map proc str1 str2 ...) returns a string built by applying proc element-wise across the strings.  proc must return a character.  R7RS 6.7.", CATEGORY);
   register_primitive("string-for-each", 2, -1, _prim_string_for_each, "",
                      "(string-for-each proc str1 str2 ...) applies proc element-wise for effect; returns an unspecified value.  R7RS 6.7.", CATEGORY);
   register_primitive("string-set!", 3, 3, _prim_string_set_bang, "",
                      "(string-set! string k char) sets character k of string to char in place.  All references to the string see the change.  R7RS 6.7.", CATEGORY);
   register_primitive("string-fill!", 2, 4, _prim_string_fill_bang, "",
                      "(string-fill! string char [start [end]]) fills string[start..end) with char.  R7RS 6.7.", CATEGORY);
   register_primitive("string-copy!", 3, 5, _prim_string_copy_bang, "",
                      "(string-copy! to at from [start [end]]) copies from[start..end) into to starting at index at.  R7RS 6.7.", CATEGORY);
   }
