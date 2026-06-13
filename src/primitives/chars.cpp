// primitives/chars.cpp -- character primitives.
// Direct port of pyscheme/primitives/chars.py.
#include "chars.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"
#include <cwctype>
#include <cwchar>
#include "../unicode_tables.h"

static const char* CATEGORY = "chars";

static SourceInfo* _src(const Value* a)
   {
   return a ? src_of(*a) : nullptr;
   }

static char32_t _check_char(const Value& v, const char* name, const Value* app, int idx = 1)
   {
   if (!is_character(v))
      throw SchemeTypeError(
          std::string(name) + ": argument " + std::to_string(idx) + " must be a character",
          _src(app));
   return as_character(v);
   }

static Value _prim_char_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_character(args[0]));
   }

static Value _char_compare(const char* name, std::vector<Value>& args, const Value* app,
                           bool (*op)(char32_t, char32_t))
   {
   char32_t prev = _check_char(args[0], name, app, 1);
   for (int i = 1; i < static_cast<int>(args.size()); ++i)
      {
      char32_t cur = _check_char(args[i], name, app, i + 1);
      if (!op(prev, cur))
         return make_boolean(false);
      prev = cur;
      }
   return make_boolean(true);
   }

static Value _prim_char_eq(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _char_compare("char=?", a, n, [](char32_t x, char32_t y)
                        { return x == y; });
   }
static Value _prim_char_lt(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _char_compare("char<?", a, n, [](char32_t x, char32_t y)
                        { return x < y; });
   }
static Value _prim_char_le(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _char_compare("char<=?", a, n, [](char32_t x, char32_t y)
                        { return x <= y; });
   }
static Value _prim_char_gt(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _char_compare("char>?", a, n, [](char32_t x, char32_t y)
                        { return x > y; });
   }
static Value _prim_char_ge(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _char_compare("char>=?", a, n, [](char32_t x, char32_t y)
                        { return x >= y; });
   }

static Value _prim_char_alphabetic(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   char32_t c = _check_char(args[0], "char-alphabetic?", app);
   return make_boolean(unicode::is_alpha(c));
   }

static Value _prim_char_numeric(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   char32_t c = _check_char(args[0], "char-numeric?", app);
   return make_boolean(unicode::is_digit(c));
   }

static Value _prim_char_whitespace(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   char32_t c = _check_char(args[0], "char-whitespace?", app);
   return make_boolean(unicode::is_space(c));
   }

static Value _prim_char_upper_case(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   char32_t c = _check_char(args[0], "char-upper-case?", app);
   return make_boolean(unicode::is_upper(c));
   }

static Value _prim_char_lower_case(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   char32_t c = _check_char(args[0], "char-lower-case?", app);
   return make_boolean(unicode::is_lower(c));
   }

static Value _prim_char_upcase(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   char32_t c = _check_char(args[0], "char-upcase", app);
   char32_t out[3];
   int n = unicode::upcase(c, out);
   return make_character(n == 1 ? out[0] : c);  // R7RS: unchanged if not 1:1
   }

static Value _prim_char_downcase(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   char32_t c = _check_char(args[0], "char-downcase", app);
   char32_t out[3];
   int n = unicode::downcase(c, out);
   return make_character(n == 1 ? out[0] : c);
   }

static Value _prim_char_foldcase(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   char32_t c = _check_char(args[0], "char-foldcase", app);
   char32_t out[3];
   int n = unicode::foldcase(c, out);
   return make_character(n == 1 ? out[0] : c);
   }

// Case-insensitive comparison: fold both to lowercase first.
static Value _char_compare_ci(const char* name, std::vector<Value>& args, const Value* app,
                              bool (*op)(char32_t, char32_t))
   {
   auto fold = [](char32_t c)
   {
      char32_t out[3];
      unicode::foldcase(c, out);
      return out[0];
   };
   char32_t prev = fold(_check_char(args[0], name, app, 1));
   for (int i = 1; i < static_cast<int>(args.size()); ++i)
      {
      char32_t cur = fold(_check_char(args[i], name, app, i + 1));
      if (!op(prev, cur))
         return make_boolean(false);
      prev = cur;
      }
   return make_boolean(true);
   }

static Value _prim_char_ci_eq(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _char_compare_ci("char-ci=?", a, n, [](char32_t x, char32_t y)
                           { return x == y; });
   }
static Value _prim_char_ci_lt(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _char_compare_ci("char-ci<?", a, n, [](char32_t x, char32_t y)
                           { return x < y; });
   }
static Value _prim_char_ci_le(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _char_compare_ci("char-ci<=?", a, n, [](char32_t x, char32_t y)
                           { return x <= y; });
   }
static Value _prim_char_ci_gt(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _char_compare_ci("char-ci>?", a, n, [](char32_t x, char32_t y)
                           { return x > y; });
   }
static Value _prim_char_ci_ge(Context*, Environment*, std::vector<Value>& a, const Value* n)
   {
   return _char_compare_ci("char-ci>=?", a, n, [](char32_t x, char32_t y)
                           { return x >= y; });
   }

static Value _prim_char_to_integer(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   char32_t c = _check_char(args[0], "char->integer", app);
   return make_integer(static_cast<int64_t>(c));
   }

static Value _prim_integer_to_char(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_integer(args[0]))
      throw SchemeTypeError("integer->char: argument must be an integer", _src(app));
   int64_t n = as_integer(args[0]);
   if (n < 0 || n > 0x10FFFF || (0xD800 <= n && n <= 0xDFFF))
      throw SchemeTypeError("integer->char: code point out of range", _src(app));
   return make_character(static_cast<char32_t>(n));
   }

static Value _prim_digit_value(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   char32_t c = _check_char(args[0], "digit-value", app);
   if (c >= U'0' && c <= U'9')
      return make_integer(static_cast<int64_t>(c - U'0'));
   // Common Unicode decimal digit blocks
   struct
      {
      char32_t base;
      } blocks[] = {
          {0x0660}, {0x06F0}, {0x07C0}, {0x0966}, {0x09E6}, {0x0A66}, {0x0AE6}, {0x0B66}, {0x0BE6}, {0x0C66}, {0x0CE6}, {0x0D66}, {0x0DE6}, {0x0E50}, {0x0ED0}, {0x0F20}, {0x1040}, {0x1090}, {0x17E0}, {0x1810}, {0x1946}, {0x19D0}, {0x1A80}, {0x1A90}, {0x1B50}, {0x1BB0}, {0x1C40}, {0x1C50}, {0xA620}, {0xA8D0}, {0xA900}, {0xA9D0}, {0xA9F0}, {0xAA50}, {0xABF0}, {0xFF10}, {0}};
   for (auto* b = blocks; b->base; ++b)
      {
      if (c >= b->base && c < b->base + 10)
         return make_integer(static_cast<int64_t>(c - b->base));
      }
   return make_boolean(false);
   }

void register_chars()
   {
   register_primitive("char?", 1, 1, _prim_char_p, "", "Return #t if obj is a character.  R7RS 6.6.", CATEGORY);
   register_primitive("char=?", 1, -1, _prim_char_eq, "", "Return #t if all character arguments compare equal.  R7RS 6.6.", CATEGORY);
   register_primitive("char<?", 1, -1, _prim_char_lt, "", "Return #t if characters are in strictly ascending order.  R7RS 6.6.", CATEGORY);
   register_primitive("char<=?", 1, -1, _prim_char_le, "", "Return #t if characters are in non-descending order.  R7RS 6.6.", CATEGORY);
   register_primitive("char>?", 1, -1, _prim_char_gt, "", "Return #t if characters are in strictly descending order.  R7RS 6.6.", CATEGORY);
   register_primitive("char>=?", 1, -1, _prim_char_ge, "", "Return #t if characters are in non-ascending order.  R7RS 6.6.", CATEGORY);
   register_primitive("char-alphabetic?", 1, 1, _prim_char_alphabetic, "", "Return #t if char is a letter.  R7RS 6.6.", CATEGORY);
   register_primitive("char-numeric?", 1, 1, _prim_char_numeric, "", "Return #t if char is a digit.  R7RS 6.6.", CATEGORY);
   register_primitive("char-whitespace?", 1, 1, _prim_char_whitespace, "", "Return #t if char is whitespace.  R7RS 6.6.", CATEGORY);
   register_primitive("char-upper-case?", 1, 1, _prim_char_upper_case, "", "Return #t if char is an upper-case letter.  R7RS 6.6.", CATEGORY);
   register_primitive("char-lower-case?", 1, 1, _prim_char_lower_case, "", "Return #t if char is a lower-case letter.  R7RS 6.6.", CATEGORY);
   register_primitive("char-upcase", 1, 1, _prim_char_upcase, "", "Return char's upper-case equivalent.  R7RS 6.6.", CATEGORY);
   register_primitive("char-downcase", 1, 1, _prim_char_downcase, "", "Return char's lower-case equivalent.  R7RS 6.6.", CATEGORY);
   register_primitive("char-foldcase", 1, 1, _prim_char_foldcase, "", "Return char's case-folded equivalent (Unicode full case folding).  R7RS 6.6.", CATEGORY);
   register_primitive("char-ci=?", 1, -1, _prim_char_ci_eq, "", "Case-insensitive char=?.  R7RS 6.6.", CATEGORY);
   register_primitive("char-ci<?", 1, -1, _prim_char_ci_lt, "", "Case-insensitive char<?.  R7RS 6.6.", CATEGORY);
   register_primitive("char-ci<=?", 1, -1, _prim_char_ci_le, "", "Case-insensitive char<=?.  R7RS 6.6.", CATEGORY);
   register_primitive("char-ci>?", 1, -1, _prim_char_ci_gt, "", "Case-insensitive char>?.  R7RS 6.6.", CATEGORY);
   register_primitive("char-ci>=?", 1, -1, _prim_char_ci_ge, "", "Case-insensitive char>=?.  R7RS 6.6.", CATEGORY);
   register_primitive("char->integer", 1, 1, _prim_char_to_integer, "", "Return char's Unicode code point as an integer.  R7RS 6.6.", CATEGORY);
   register_primitive("integer->char", 1, 1, _prim_integer_to_char, "", "Return the character with the given Unicode code point.  R7RS 6.6.", CATEGORY);
   register_primitive("digit-value", 1, 1, _prim_digit_value, "",
                      "Return the numeric value of a digit character, or #f if char is not a digit.  R7RS 6.6.", CATEGORY);
   }
