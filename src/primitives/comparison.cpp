// primitives/comparison.cpp -- numeric comparison primitives.
// Direct port of pyscheme/primitives/comparison.py.
#include "comparison.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"
#include "../rational.h"
#include <complex>
#include <variant>
#include <cmath>
#include <cstdio>
#include <cinttypes>
#include <vector>

static const char* CATEGORY = "comparison";

// ── Exact comparison (bignum-aware) ───────────────────────────────────────────
// NumReal/NumAny collapse a bignum to double on extraction, which makes two
// bignums that round to the same double compare equal (e.g. 10^50 and 10^50+1).
// When every argument is an exact real (fixnum, bignum, or rational) and at
// least one is a bignum, the chain is compared exactly via mpz instead, mirroring
// pyscheme (whose ints are arbitrary precision).

static void _cmp_set_i64(__mpz_struct* z, int64_t n)
   {
   // mpz_set_si takes a 32-bit long on MSVC, so route int64 through a string.
   char buf[24];
   if (n < 0)
      std::snprintf(buf, sizeof(buf), "-%" PRIu64, static_cast<uint64_t>(-n));
   else
      std::snprintf(buf, sizeof(buf), "%" PRIu64, static_cast<uint64_t>(n));
   mpz_set_str(z, buf, 10);
   }

static bool _is_exact_real(const Value& v)
   {
   return is_integer(v) || is_bignum(v) || is_rational(v);
   }

// True when every arg is an exact real and at least one is a bignum (the only
// case where the default double-collapsing path can give a wrong answer).
static bool _exact_chain_applies(const std::vector<Value>& args)
   {
   bool has_bignum = false;
   for (const Value& v : args)
      {
      if (is_bignum(v))
         has_bignum = true;
      else if (!_is_exact_real(v))
         return false;
      }
   return has_bignum;
   }

// Set (num, den) from an exact real; den is always > 0.  Caller owns the mpz.
static void _exact_to_ratio(const Value& v, __mpz_struct* num, __mpz_struct* den)
   {
   mpz_init(num);
   mpz_init(den);
   if (is_bignum(v))
      {
      mpz_set(num, as_bignum(v));
      _cmp_set_i64(den, 1);
      }
   else if (is_integer(v))
      {
      _cmp_set_i64(num, as_integer(v));
      _cmp_set_i64(den, 1);
      }
   else // rational: int64 numerator/denominator, denominator normalized positive
      {
      _cmp_set_i64(num, as_rational_num(v));
      _cmp_set_i64(den, as_rational_den(v));
      }
   }

// Exact sign of (a - b): -1, 0, or 1.  Both args must be exact reals.
static int _exact_cmp(const Value& a, const Value& b)
   {
   __mpz_struct an, ad, bn, bd, l, r;
   _exact_to_ratio(a, &an, &ad);
   _exact_to_ratio(b, &bn, &bd);
   mpz_init(&l);
   mpz_init(&r);
   mpz_mul(&l, &an, &bd); // a.num * b.den   (denominators > 0, sign preserved)
   mpz_mul(&r, &bn, &ad); // b.num * a.den
   int c = mpz_cmp(&l, &r);
   mpz_clear(&an);
   mpz_clear(&ad);
   mpz_clear(&bn);
   mpz_clear(&bd);
   mpz_clear(&l);
   mpz_clear(&r);
   return c;
   }

template <class Pred>
static Value _exact_chain(const std::vector<Value>& args, Pred ok)
   {
   for (size_t i = 1; i < args.size(); ++i)
      if (!ok(_exact_cmp(args[i - 1], args[i])))
         return make_boolean(false);
   return make_boolean(true);
   }

// ── NumReal: real-valued number extracted from a Value (for ordering) ─────────
// Port of comparison.py _num.

using NumReal = std::variant<int64_t, Rat, double>;

static double to_double(const NumReal& n)
   {
   return std::visit([](auto v)
                     { return static_cast<double>(v); }, n);
   }

static bool real_lt(const NumReal& a, const NumReal& b)
   {
   if (auto* ai = std::get_if<int64_t>(&a))
      {
      if (auto* bi = std::get_if<int64_t>(&b))
         return *ai < *bi;
      if (auto* br = std::get_if<Rat>(&b))
         return *ai < *br;
      }
   if (auto* ar = std::get_if<Rat>(&a))
      {
      if (auto* bi = std::get_if<int64_t>(&b))
         return *ar < *bi;
      if (auto* br = std::get_if<Rat>(&b))
         return *ar < *br;
      }
   return to_double(a) < to_double(b);
   }

static bool real_le(const NumReal& a, const NumReal& b)
   {
   if (auto* ai = std::get_if<int64_t>(&a))
      {
      if (auto* bi = std::get_if<int64_t>(&b))
         return *ai <= *bi;
      if (auto* br = std::get_if<Rat>(&b))
         return *ai <= *br;
      }
   if (auto* ar = std::get_if<Rat>(&a))
      {
      if (auto* bi = std::get_if<int64_t>(&b))
         return *ar <= *bi;
      if (auto* br = std::get_if<Rat>(&b))
         return *ar <= *br;
      }
   return to_double(a) <= to_double(b);
   }

static bool real_eq(const NumReal& a, const NumReal& b)
   {
   if (auto* ai = std::get_if<int64_t>(&a))
      {
      if (auto* bi = std::get_if<int64_t>(&b))
         return *ai == *bi;
      if (auto* br = std::get_if<Rat>(&b))
         return *ai == *br;
      }
   if (auto* ar = std::get_if<Rat>(&a))
      {
      if (auto* bi = std::get_if<int64_t>(&b))
         return *ar == *bi;
      if (auto* br = std::get_if<Rat>(&b))
         return *ar == *br;
      }
   return to_double(a) == to_double(b);
   }

static NumReal _num(const Value& v, const char* name, const Value* app_node, int i)
   {
   if (is_integer(v))
      return as_integer(v);
   if (is_bignum(v))
      return mpz_get_d(as_bignum(v));
   if (is_rational(v))
      return Rat{as_rational_num(v), as_rational_den(v)};
   if (is_real(v))
      return as_real(v);
   if (is_complex(v) && as_complex_imag(v) == 0.0)
      return as_complex_real(v);
   if (is_exact_complex(v))
      {
      Value im = as_exact_complex_imag(v);
      if (is_integer(im) && as_integer(im) == 0)
         {
         Value re = as_exact_complex_real(v);
         if (is_integer(re))
            return as_integer(re);
         if (is_rational(re))
            return Rat{as_rational_num(re), as_rational_den(re)};
         }
      }
   throw SchemeTypeError(
       std::string(name) + ": argument " + std::to_string(i) + " is not a real number",
       app_node ? src_of(*app_node) : nullptr);
   }

// ── NumAny: any number (including complex) for = ──────────────────────────────
// Port of comparison.py _num_eq helper.

using NumAny = std::variant<int64_t, Rat, double, std::complex<double>>;

static NumAny _num_eq_val(const Value& v, const Value* app_node, int i)
   {
   if (is_integer(v))
      return as_integer(v);
   if (is_bignum(v))
      return mpz_get_d(as_bignum(v));
   if (is_rational(v))
      return Rat{as_rational_num(v), as_rational_den(v)};
   if (is_real(v))
      return as_real(v);
   if (is_complex(v))
      return std::complex<double>(as_complex_real(v), as_complex_imag(v));
   if (is_exact_complex(v))
      {
      Value re = as_exact_complex_real(v);
      Value im = as_exact_complex_imag(v);
      double re_d = is_integer(re) ? static_cast<double>(as_integer(re))
                                   : static_cast<double>(Rat{as_rational_num(re), as_rational_den(re)});
      double im_d = is_integer(im) ? static_cast<double>(as_integer(im))
                                   : static_cast<double>(Rat{as_rational_num(im), as_rational_den(im)});
      return std::complex<double>(re_d, im_d);
      }
   throw SchemeTypeError(
       std::string("=: argument ") + std::to_string(i) + " is not a number",
       app_node ? src_of(*app_node) : nullptr);
   }

static bool any_eq(const NumAny& a, const NumAny& b)
   {
   // Convert both to complex<double> for equality check -- same as Python's cross-type ==.
   auto to_cx = [](const NumAny& n) -> std::complex<double>
   {
      return std::visit([](auto v) -> std::complex<double>
                        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::complex<double>>)
                return v;
            else
                return { static_cast<double>(v), 0.0 }; }, n);
   };
   // Exact int/rat comparisons first.
   if (auto* ai = std::get_if<int64_t>(&a))
      {
      if (auto* bi = std::get_if<int64_t>(&b))
         return *ai == *bi;
      if (auto* br = std::get_if<Rat>(&b))
         return *ai == *br;
      }
   if (auto* ar = std::get_if<Rat>(&a))
      {
      if (auto* bi = std::get_if<int64_t>(&b))
         return *ar == *bi;
      if (auto* br = std::get_if<Rat>(&b))
         return *ar == *br;
      }
   // Exact integer vs inexact real: compare exactly, mirroring Python's int==float
   // (which never rounds the integer to fit a double).  Without this, distinct
   // values that share a double rounding collide, e.g.
   // (= 9007199254740992.0 9007199254740993) would wrongly return #t.
   auto int_eq_double = [](int64_t i, double d) -> bool
   {
      if (!std::isfinite(d) || std::floor(d) != d)
         return false;                                  // non-finite or non-integral
      if (d < -9223372036854775808.0 || d >= 9223372036854775808.0)
         return false;                                  // outside int64 range -> unequal
      return static_cast<int64_t>(d) == i;
   };
   if (auto* ai = std::get_if<int64_t>(&a))
      if (auto* bd = std::get_if<double>(&b))
         return int_eq_double(*ai, *bd);
   if (auto* ad = std::get_if<double>(&a))
      if (auto* bi = std::get_if<int64_t>(&b))
         return int_eq_double(*bi, *ad);
   return to_cx(a) == to_cx(b);
   }

// ── Primitives ────────────────────────────────────────────────────────────────

// Monotonic pairwise comparison chain shared by =, <, >, <=, >=.  T is the
// extracted element type -- NumAny for = (admits complex), NumReal for the
// orderings; extract pulls a T from each arg and op is the pairwise predicate.
// Mirrors comparison.py _num_compare (and the char_compare pattern in chars.cpp).
template <class T, class Extract, class Op>
static Value num_compare(std::vector<Value>& args, const Value* app,
                         Extract extract, Op op)
   {
   T prev = extract(args[0], app, 1);
   for (int i = 1; i < static_cast<int>(args.size()); ++i)
      {
      T cur = extract(args[i], app, i + 1);
      if (!op(prev, cur))
         return make_boolean(false);
      prev = cur;
      }
   return make_boolean(true);
   }

static Value _prim_num_eq(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (_exact_chain_applies(args))
      return _exact_chain(args, [](int c) { return c == 0; });
   return num_compare<NumAny>(args, app,
       [](const Value& v, const Value* a, int i) { return _num_eq_val(v, a, i); },
       [](const NumAny& x, const NumAny& y) { return any_eq(x, y); });
   }

static Value _prim_num_lt(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (_exact_chain_applies(args))
      return _exact_chain(args, [](int c) { return c < 0; });
   return num_compare<NumReal>(args, app,
       [](const Value& v, const Value* a, int i) { return _num(v, "<", a, i); },
       [](const NumReal& x, const NumReal& y) { return real_lt(x, y); });
   }

static Value _prim_num_gt(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (_exact_chain_applies(args))
      return _exact_chain(args, [](int c) { return c > 0; });
   return num_compare<NumReal>(args, app,
       [](const Value& v, const Value* a, int i) { return _num(v, ">", a, i); },
       [](const NumReal& x, const NumReal& y) { return real_lt(y, x); });
   }

static Value _prim_num_le(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (_exact_chain_applies(args))
      return _exact_chain(args, [](int c) { return c <= 0; });
   return num_compare<NumReal>(args, app,
       [](const Value& v, const Value* a, int i) { return _num(v, "<=", a, i); },
       [](const NumReal& x, const NumReal& y) { return real_le(x, y); });
   }

static Value _prim_num_ge(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (_exact_chain_applies(args))
      return _exact_chain(args, [](int c) { return c >= 0; });
   return num_compare<NumReal>(args, app,
       [](const Value& v, const Value* a, int i) { return _num(v, ">=", a, i); },
       [](const NumReal& x, const NumReal& y) { return real_le(y, x); });
   }

void register_comparison()
   {
   register_primitive("=", 1, -1, _prim_num_eq,
                      "", "Return #t if all arguments are numerically equal.", CATEGORY);
   register_primitive("<", 1, -1, _prim_num_lt,
                      "", "Return #t if the arguments are monotonically strictly increasing.", CATEGORY);
   register_primitive(">", 1, -1, _prim_num_gt,
                      "", "Return #t if the arguments are monotonically strictly decreasing.", CATEGORY);
   register_primitive("<=", 1, -1, _prim_num_le,
                      "", "Return #t if the arguments are monotonically non-decreasing.", CATEGORY);
   register_primitive(">=", 1, -1, _prim_num_ge,
                      "", "Return #t if the arguments are monotonically non-increasing.", CATEGORY);
   }
