// primitives/arithmetic.cpp -- arithmetic primitives.
// Direct port of pyscheme/primitives/arithmetic.py.
#include "arithmetic.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"
#include "../rational.h"
#define _USE_MATH_DEFINES
#include <cmath>
#include <complex>
#include <numeric>
#include <random>
#include <algorithm>
#include <string>
#include <stdexcept>
#include <variant>
#include <limits>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <charconv>
#include <cinttypes>
#include <climits>
#include <cerrno>

static const char* CATEGORY = "arithmetic";
static SourceInfo* _src(const Value* a)
   {
   return a ? src_of(*a) : nullptr;
   }

// ── Bignum helpers ────────────────────────────────────────────────────────────
// Set mpz from int64_t (works around MSVC's 32-bit long).

static void _mpz_set_i64(__mpz_struct* z, int64_t n)
   {
   char buf[24];
   if (n < 0)
      std::snprintf(buf, sizeof(buf), "-%" PRIu64, static_cast<uint64_t>(-n));
   else
      std::snprintf(buf, sizeof(buf), "%" PRIu64, static_cast<uint64_t>(n));
   mpz_set_str(z, buf, 10);
   }

// Overflow-safe int64 add: returns true if overflow occurred.
static bool _i64_add(int64_t a, int64_t b, int64_t& out)
   {
   uint64_t ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b), ur = ua + ub;
   out = static_cast<int64_t>(ur);
   return ((a ^ out) & (b ^ out)) < 0;
   }
static bool _i64_sub(int64_t a, int64_t b, int64_t& out)
   {
   uint64_t ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b), ur = ua - ub;
   out = static_cast<int64_t>(ur);
   return ((a ^ b) & (a ^ out)) < 0;
   }
static bool _i64_mul(int64_t a, int64_t b, int64_t& out)
   {
   if (a == 0 || b == 0)
      {
      out = 0;
      return false;
      }
   if (a == -1)
      {
      out = -b;
      return (b == INT64_MIN);
      }
   if (b == -1)
      {
      out = -a;
      return (a == INT64_MIN);
      }
   // Use 128-bit check where available; fall back to divide.
#if defined(__GNUC__) || defined(__clang__)
   __int128 r = (__int128)a * b;
   out = (int64_t)r;
   return r != (__int128)out;
#else
   out = a * b;
   return (b != 0 && out / b != a);
#endif
   }

// Convert an int64_t or bignum Value to an mpz (caller owns z).
static void _val_to_mpz(__mpz_struct* z, const Value& v)
   {
   mpz_init(z);
   if (is_integer(v))
      _mpz_set_i64(z, as_integer(v));
   else
      mpz_set(z, as_bignum(v));
   }

// Return make_integer if mpz fits, else make_bignum_copy.
static Value _mpz_to_value(const __mpz_struct* z)
   {
   // mpz_sizeinbase can overestimate by 1; values with <=63 reported bits
   // may still fit in int64_t.  Use string conversion to avoid mpz_get_si
   // truncation on Windows where `long` is 32-bit (LLP64 model).
   if (mpz_sizeinbase(z, 2) <= 63)
      {
      char buf[24];
      mpz_get_str(buf, 10, z);
      errno = 0;
      int64_t n = std::strtoll(buf, nullptr, 10);
      if (errno == 0)
         return make_integer(n);
      }
   return make_bignum_copy(z);
   }

// ── NumAny: exact-real accumulator (int64_t | Rat | double) ─────────────────
using NumAny = std::variant<int64_t, Rat, double>;

// ── CplxResult: decomposed complex number ───────────────────────────────────
struct CplxResult
   {
   NumAny re = int64_t(0);
   NumAny im = int64_t(0);
   bool exact = true;
   bool valid = true; // false = non-numeric input
   };

// ── IntXResult: result of _check_int_x ──────────────────────────────────────
struct IntXResult
   {
   int64_t re;
   bool exact;
   };

// ── Exact component helpers ──────────────────────────────────────────────────

static Rat _as_exact_component(const Value& v)
   {
   if (is_integer(v))
      return Rat(as_integer(v));
   return Rat(as_rational_num(v), as_rational_den(v));
   }

static Value _wrap_component(const Rat& r)
   {
   if (r.denominator == 1)
      return make_integer(r.numerator);
   return make_rational(r.numerator, r.denominator);
   }

// ── NumAny operations ────────────────────────────────────────────────────────
// The Rat × int64_t and Rat × double operators (and reverses) are defined in
// rational.h, so x op y for auto x,y resolves to the correct return type via
// ADL: Rat+Rat→Rat, Rat+int64_t→Rat, Rat+double→double, int64_t+int64_t→int64_t.

static double _numany_to_double(const NumAny& n)
   {
   return std::visit([](auto x) -> double
                     { return static_cast<double>(x); }, n);
   }

static Rat _numany_to_rat(const NumAny& n)
   {
   return std::visit([](auto x) -> Rat
                     {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, int64_t>) return Rat(x);
        else if constexpr (std::is_same_v<T, Rat>)  return x;
        else                                          return Rat::from_float(x); }, n);
   }

static NumAny _numany_add(const NumAny& a, const NumAny& b)
   {
   return std::visit([](auto x, auto y) -> NumAny
                     { return x + y; }, a, b);
   }

static NumAny _numany_sub(const NumAny& a, const NumAny& b)
   {
   return std::visit([](auto x, auto y) -> NumAny
                     { return x - y; }, a, b);
   }

static NumAny _numany_mul(const NumAny& a, const NumAny& b)
   {
   return std::visit([](auto x, auto y) -> NumAny
                     { return x * y; }, a, b);
   }

static NumAny _numany_neg(const NumAny& a)
   {
   return std::visit([](auto x) -> NumAny
                     { return -x; }, a);
   }

static bool _numany_lt(const NumAny& a, const NumAny& b)
   {
   return std::visit([](auto x, auto y) -> bool
                     { return x < y; }, a, b);
   }

static bool _is_exact_zero(const NumAny& n)
   {
   if (auto* i = std::get_if<int64_t>(&n))
      return *i == 0;
   if (auto* r = std::get_if<Rat>(&n))
      return r->numerator == 0;
   return false;
   }

// ── Value wrappers ───────────────────────────────────────────────────────────

static Value _wrap_numany(const NumAny& n)
   {
   return std::visit([](auto x) -> Value
                     {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, int64_t>)
            return make_integer(x);
        else if constexpr (std::is_same_v<T, Rat>) {
            if (x.denominator == 1) return make_integer(x.numerator);
            return make_rational(x.numerator, x.denominator);
        } else {
            return make_real(x);
        } }, n);
   }

static Value _component_to_value(const NumAny& n)
   {
   return std::visit([](auto x) -> Value
                     {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, int64_t>)
            return make_integer(x);
        else if constexpr (std::is_same_v<T, Rat>) {
            if (x.denominator == 1) return make_integer(x.numerator);
            return make_rational(x.numerator, x.denominator);
        } else {
            return make_real(x);
        } }, n);
   }

static bool _numany_is_zero(const NumAny& n)
   {
   return std::visit([](auto x) -> bool
                     {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, int64_t>) return x == int64_t(0);
        else if constexpr (std::is_same_v<T, Rat>)  return x == int64_t(0);
        else                                          return x == 0.0; }, n);
   }

static Value _wrap_cplx_result(const NumAny& re, const NumAny& im, bool exact)
   {
   if (exact)
      {
      if (_numany_is_zero(im))
         return _wrap_numany(re);
      return make_exact_complex(_component_to_value(re), _component_to_value(im));
      }
   else
      {
      double re_f = _numany_to_double(re);
      double im_f = _numany_to_double(im);
      if (im_f == 0.0)
         return make_real(re_f);
      return make_complex(re_f, im_f);
      }
   }

// ── Complex helpers ──────────────────────────────────────────────────────────

static const double _MACH_EPS = 2.2204460492503131e-16;

static bool _is_complex_type(const Value& v)
   {
   return is_complex(v) || is_exact_complex(v);
   }

static std::complex<double> _val_to_complex(const Value& v)
   {
   if (is_complex(v))
      return {as_complex_real(v), as_complex_imag(v)};
   if (is_exact_complex(v))
      {
      return {static_cast<double>(_as_exact_component(as_exact_complex_real(v))),
              static_cast<double>(_as_exact_component(as_exact_complex_imag(v)))};
      }
   if (is_integer(v))
      return {static_cast<double>(as_integer(v)), 0.0};
   if (is_rational(v))
      return {static_cast<double>(Rat(as_rational_num(v), as_rational_den(v))), 0.0};
   if (is_real(v))
      return {as_real(v), 0.0};
   return {0.0, 0.0};
   }

static Value _wrap_cplx(const std::complex<double>& c)
   {
   if (c.imag() == 0.0)
      return make_real(c.real());
   return make_complex(c.real(), c.imag());
   }

static std::complex<double> _clean_complex(const std::complex<double>& c)
   {
   if (std::abs(c.imag()) == 1.0 && std::abs(c.real()) <= _MACH_EPS)
      return {0.0, c.imag()};
   return c;
   }

// Raise complex base to an integer power using repeated squaring.
// More accurate than std::pow(cb, int) which may use exp(n*log(z)) internally.
static std::complex<double> _cpow_int(std::complex<double> base, int64_t n)
   {
   if (n == 0)
      return {1.0, 0.0};
   bool neg = n < 0;
   if (neg)
      n = -n;
   std::complex<double> result{1.0, 0.0};
   for (; n > 0; n >>= 1)
      {
      if (n & 1)
         result *= base;
      base *= base;
      }
   return neg ? std::complex<double>{1.0, 0.0} / result : result;
   }

// ── Number extractors ────────────────────────────────────────────────────────

// Port of _num: real only (raises for complex).
static NumAny _num_real(const Value& v, const char* name, const Value* app, int i)
   {
   if (is_integer(v))
      return as_integer(v);
   if (is_bignum(v))
      return mpz_get_d(as_bignum(v));
   if (is_rational(v))
      return Rat(as_rational_num(v), as_rational_den(v));
   if (is_real(v))
      return as_real(v);
   throw SchemeTypeError(
       std::string(name) + ": argument " + std::to_string(i) + " is not a real number",
       _src(app));
   }

// Port of _any_num: real or complex. Returns NumAny (double approximation for complex).
static NumAny _any_num(const Value& v, const char* name, const Value* app, int i)
   {
   if (is_integer(v))
      return as_integer(v);
   if (is_bignum(v))
      return mpz_get_d(as_bignum(v));
   if (is_rational(v))
      return Rat(as_rational_num(v), as_rational_den(v));
   if (is_real(v))
      return as_real(v);
   if (is_complex(v))
      return as_complex_real(v); // float approximation
   if (is_exact_complex(v))
      return static_cast<double>(_as_exact_component(as_exact_complex_real(v)));
   throw SchemeTypeError(
       std::string(name) + ": argument " + std::to_string(i) + " is not a number",
       _src(app));
   }

// Port of _extract_complex.
static CplxResult _extract_complex(const Value& v)
   {
   if (is_exact_complex(v))
      return {_as_exact_component(as_exact_complex_real(v)),
              _as_exact_component(as_exact_complex_imag(v)), true, true};
   if (is_complex(v))
      return {as_complex_real(v), as_complex_imag(v), false, true};
   if (is_integer(v))
      return {as_integer(v), int64_t(0), true, true};
   if (is_rational(v))
      return {Rat(as_rational_num(v), as_rational_den(v)), int64_t(0), true, true};
   if (is_real(v))
      return {as_real(v), 0.0, false, true};
   return {int64_t(0), int64_t(0), true, false};
   }

static bool _has_any_complex(const std::vector<Value>& args)
   {
   for (const auto& a : args)
      if (is_complex(a) || is_exact_complex(a))
         return true;
   return false;
   }

// ── Integer helpers ──────────────────────────────────────────────────────────

static IntXResult _check_int_x(const Value& v, const char* name, const Value* app, int i)
   {
   if (is_integer(v))
      return {as_integer(v), true};
   if (is_real(v))
      {
      double fv = as_real(v);
      if (std::isfinite(fv) && fv == std::trunc(fv))
         return {static_cast<int64_t>(fv), false};
      }
   throw SchemeTypeError(
       std::string(name) + ": argument " + std::to_string(i) + " is not an integer",
       _src(app));
   }

static int64_t _check_int(const Value& v, const char* name, const Value* app, int i)
   {
   if (is_integer(v))
      return as_integer(v);
   throw SchemeTypeError(
       std::string(name) + ": argument " + std::to_string(i) + " is not an integer",
       _src(app));
   }

// Port of _trunc_div: C++ integer / already truncates toward zero (C++11 guarantee).
static int64_t _trunc_div(int64_t n, int64_t d)
   {
   return n / d;
   }

// ── Division helpers ─────────────────────────────────────────────────────────

// Port of _exact_div for non-complex NumAny operands.
static NumAny _exact_div_numany(const NumAny& a, const NumAny& b)
   {
   return std::visit([](auto x, auto y) -> NumAny
                     {
        using X = std::decay_t<decltype(x)>;
        using Y = std::decay_t<decltype(y)>;
        if constexpr (std::is_same_v<Y, double>) {
            double fb = y, fa = static_cast<double>(x);
            if (fb == 0.0) {
                if (std::isnan(fa) || fa == 0.0) return std::numeric_limits<double>::quiet_NaN();
                return std::copysign(std::numeric_limits<double>::infinity(), fa);
            }
            return fa / fb;
        } else if constexpr (std::is_same_v<X, double>) {
            return x / static_cast<double>(y);
        } else {
            // Both exact: use Rat for exact rational result.
            return Rat(x) / Rat(y);
        } }, a, b);
   }

// Port of _complex_div.
static CplxResult _complex_div(const CplxResult& a, const CplxResult& b)
   {
   bool exact = a.exact && b.exact;
   if (exact)
      {
      Rat are = _numany_to_rat(a.re), aim = _numany_to_rat(a.im);
      Rat bre = _numany_to_rat(b.re), bim = _numany_to_rat(b.im);
      Rat denom = bre * bre + bim * bim;
      if (denom.numerator == 0)
         return {int64_t(0), int64_t(0), true, false};
      Rat re = (are * bre + aim * bim) / denom;
      Rat im = (aim * bre - are * bim) / denom;
      NumAny re_n = (re.denominator == 1) ? NumAny{re.numerator} : NumAny{re};
      NumAny im_n = (im.denominator == 1) ? NumAny{im.numerator} : NumAny{im};
      return {re_n, im_n, true, true};
      }
   else
      {
      double are = _numany_to_double(a.re), aim = _numany_to_double(a.im);
      double bre = _numany_to_double(b.re), bim = _numany_to_double(b.im);
      double denom = bre * bre + bim * bim;
      if (denom == 0.0)
         return {int64_t(0), int64_t(0), true, false};
      NumAny re{(are * bre + aim * bim) / denom};
      NumAny im{(aim * bre - are * bim) / denom};
      return {re, im, false, true};
      }
   }

// ── Number-to-string helpers ─────────────────────────────────────────────────

static std::string _format_real(double f)
   {
   if (std::isnan(f))
      return "+nan.0";
   if (f == std::numeric_limits<double>::infinity())
      return "+inf.0";
   if (f == -std::numeric_limits<double>::infinity())
      return "-inf.0";
   // Port of Python repr(f): shortest round-trip decimal.
   char buf[64];
   std::string best;
   for (int prec = 1; prec <= 17; ++prec)
      {
      int n = std::snprintf(buf, sizeof(buf), "%.*g", prec, f);
      if (n <= 0)
         continue;
      double check = 0.0;
      auto res = std::from_chars(buf, buf + n, check);
      if (res.ec == std::errc{} && check == f)
         {
         best = std::string(buf, n);
         break;
         }
      }
   if (best.empty())
      {
      int n = std::snprintf(buf, sizeof(buf), "%.17g", f);
      best = std::string(buf, n);
      }
   // For |f| in [1e-4, 1e16), Python prefers decimal notation over scientific.
   double af = f < 0.0 ? -f : f;
   if (best.find('e') != std::string::npos && af >= 1e-4 && af < 1e16)
      {
      int n = std::snprintf(buf, sizeof(buf), "%.17g", f);
      std::string d(buf, n);
      auto dp = d.find('.');
      if (dp != std::string::npos)
         {
         size_t last = d.size();
         while (last > dp + 2 && d[last - 1] == '0')
            --last;
         d.resize(last);
         }
      else
         {
         d += ".0";
         }
      best = d;
      }
   if (best.find('.') == std::string::npos && best.find('e') == std::string::npos)
      best += ".0";
   return best;
   }

static std::string _int_to_string(int64_t n, int radix)
   {
   if (n == 0)
      return "0";
   bool neg = n < 0;
   uint64_t u = neg ? (uint64_t)(-(n + 1)) + 1 : (uint64_t)n;
   std::string result;
   while (u > 0)
      {
      int d = (int)(u % (uint64_t)radix);
      result += (char)(d < 10 ? '0' + d : 'a' + d - 10);
      u /= (uint64_t)radix;
      }
   if (neg)
      result += '-';
   std::reverse(result.begin(), result.end());
   return result;
   }

// ── string->number helpers ───────────────────────────────────────────────────

static bool _parse_real_stn(const std::string& s, double& out)
   {
   if (s == "+inf.0")
      {
      out = std::numeric_limits<double>::infinity();
      return true;
      }
   if (s == "-inf.0")
      {
      out = -std::numeric_limits<double>::infinity();
      return true;
      }
   if (s == "+nan.0" || s == "-nan.0")
      {
      out = std::numeric_limits<double>::quiet_NaN();
      return true;
      }
   try
      {
      size_t pos;
      out = std::stod(s, &pos);
      return pos == s.size();
      }
   catch (...)
      {
      return false;
      }
   }

static bool _parse_complex_stn(const std::string& s, double& re_out, double& im_out)
   {
   // s ends in 'i', length >= 2
   std::string body = s.substr(0, s.size() - 1);
   if (body.empty())
      return false;
   int split = -1;
   for (int j = (int)body.size() - 1; j >= 0; --j)
      {
      char c = body[j];
      if (c == '+' || c == '-')
         {
         if (j > 0 && std::tolower((unsigned char)body[j - 1]) == 'e')
            {
            --j;
            continue;
            }
         split = j;
         break;
         }
      }
   if (split == -1)
      return false;
   std::string real_str = body.substr(0, split);
   std::string sign_imag = body.substr(split);
   double re = 0.0;
   if (!real_str.empty() && !_parse_real_stn(real_str, re))
      return false;
   double im;
   if (sign_imag == "+")
      im = 1.0;
   else if (sign_imag == "-")
      im = -1.0;
   else if (!_parse_real_stn(sign_imag, im))
      return false;
   re_out = re;
   im_out = im;
   return true;
   }

// ── Exact/inexact helpers ────────────────────────────────────────────────────

static Value _float_to_exact(double f, const char* name, const Value* app)
   {
   if (!std::isfinite(f))
      throw SchemeTypeError(std::string(name) + ": no exact representation for non-finite real", _src(app));
   if (f == std::trunc(f))
      return make_integer(static_cast<int64_t>(f));
   Rat frac = Rat::from_float(f);
   return make_rational(frac.numerator, frac.denominator);
   }

// Banker's rounding (round half to even), matching Python's round().
static double _banker_round(double f)
   {
   double r = std::round(f);
   if (std::abs(f - r) == 0.5 && std::fmod(r, 2.0) != 0.0)
      r -= std::copysign(1.0, r);
   return r;
   }

// ── Rationalize helpers ──────────────────────────────────────────────────────

static Rat _simplest_rational(Rat lo, Rat hi)
   {
   if (lo == hi)
      return lo;
   int64_t n = rat_ceil(lo);
   if (Rat(n) <= hi)
      return Rat(n);
   return Rat(1) / _simplest_rational(Rat(1) / hi, Rat(1) / lo);
   }

static Rat _rationalize_exact(Rat x, Rat delta)
   {
   Rat lo = x - delta, hi = x + delta;
   if (lo <= Rat(0) && hi >= Rat(0))
      return Rat(0);
   if (lo > Rat(0))
      return _simplest_rational(lo, hi);
   return -_simplest_rational(-hi, -lo);
   }

// ── Arithmetic primitives ────────────────────────────────────────────────────

static Value _prim_add(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (_has_any_complex(args))
      {
      NumAny acc_re{int64_t(0)}, acc_im{int64_t(0)};
      bool acc_exact = true;
      for (size_t i = 0; i < args.size(); ++i)
         {
         CplxResult a = _extract_complex(args[i]);
         if (!a.valid)
            throw SchemeTypeError("+: argument " + std::to_string(i + 1) + " is not a number", _src(app));
         if (!a.exact)
            acc_exact = false;
         acc_re = _numany_add(acc_re, a.re);
         acc_im = _numany_add(acc_im, a.im);
         }
      return _wrap_cplx_result(acc_re, acc_im, acc_exact);
      }
   // Fast path: all args are exact integers (no rational, no real, no bignum yet).
   bool all_fixnum = true;
   for (auto& a : args)
      if (!is_integer(a))
         {
         all_fixnum = false;
         break;
         }
   if (all_fixnum)
      {
      int64_t sum = 0;
      for (size_t i = 0; i < args.size(); ++i)
         {
         int64_t out;
         if (_i64_add(sum, as_integer(args[i]), out))
            {
            // Overflow: redo from scratch with bignum.
            __mpz_struct acc;
            mpz_init_set_si(&acc, 0);
            for (auto& a : args)
               {
               __mpz_struct tmp;
               _val_to_mpz(&tmp, a);
               mpz_add(&acc, &acc, &tmp);
               mpz_clear(&tmp);
               }
            Value result = _mpz_to_value(&acc);
            mpz_clear(&acc);
            return result;
            }
         sum = out;
         }
      return make_integer(sum);
      }
   // Mixed path: if any arg is bignum, accumulate in mpz.
   bool has_bignum = false;
   for (auto& a : args)
      if (is_bignum(a))
         {
         has_bignum = true;
         break;
         }
   if (has_bignum)
      {
      // If mix of bignum/fixnum only, use mpz.
      bool all_int = true;
      for (auto& a : args)
         if (!is_integer(a) && !is_bignum(a))
            {
            all_int = false;
            break;
            }
      if (all_int)
         {
         __mpz_struct acc;
         mpz_init_set_si(&acc, 0);
         for (auto& a : args)
            {
            __mpz_struct tmp;
            _val_to_mpz(&tmp, a);
            mpz_add(&acc, &acc, &tmp);
            mpz_clear(&tmp);
            }
         Value result = _mpz_to_value(&acc);
         mpz_clear(&acc);
         return result;
         }
      }
   NumAny total{int64_t(0)};
   for (size_t i = 0; i < args.size(); ++i)
      total = _numany_add(total, _any_num(args[i], "+", app, (int)(i + 1)));
   return _wrap_numany(total);
   }

static Value _prim_sub(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (_has_any_complex(args))
      {
      CplxResult acc = _extract_complex(args[0]);
      if (!acc.valid)
         throw SchemeTypeError("-: argument 1 is not a number", _src(app));
      if (args.size() == 1)
         return _wrap_cplx_result(_numany_neg(acc.re), _numany_neg(acc.im), acc.exact);
      for (size_t i = 1; i < args.size(); ++i)
         {
         CplxResult a = _extract_complex(args[i]);
         if (!a.valid)
            throw SchemeTypeError("-: argument " + std::to_string(i + 1) + " is not a number", _src(app));
         if (!a.exact)
            acc.exact = false;
         acc.re = _numany_sub(acc.re, a.re);
         acc.im = _numany_sub(acc.im, a.im);
         }
      return _wrap_cplx_result(acc.re, acc.im, acc.exact);
      }
   // Fast path: all exact integers.
   auto all_fixnum_or_bignum = [&]()
   {
      for (auto& a : args)
         if (!is_integer(a) && !is_bignum(a))
            return false;
      return true;
   };
   if (all_fixnum_or_bignum())
      {
      if (args.size() == 1)
         {
         if (is_integer(args[0]))
            {
            int64_t n = as_integer(args[0]);
            if (n != INT64_MIN)
               return make_integer(-n);
            return make_bignum_si(-static_cast<double>(n) > 0 ? 1 : -1); // promote
            }
         __mpz_struct z;
         _val_to_mpz(&z, args[0]);
         mpz_neg(&z, &z);
         Value r = _mpz_to_value(&z);
         mpz_clear(&z);
         return r;
         }
      __mpz_struct acc;
      _val_to_mpz(&acc, args[0]);
      bool needed_bignum = is_bignum(args[0]);
      // Try fast int64 first if all fixnum.
      if (!needed_bignum)
         {
         int64_t result = as_integer(args[0]);
         bool overflow = false;
         for (size_t i = 1; i < args.size(); ++i)
            {
            if (is_bignum(args[i]))
               {
               overflow = true;
               break;
               }
            int64_t out;
            if (_i64_sub(result, as_integer(args[i]), out))
               {
               overflow = true;
               break;
               }
            result = out;
            }
         if (!overflow)
            {
            mpz_clear(&acc);
            return make_integer(result);
            }
         }
      for (size_t i = 1; i < args.size(); ++i)
         {
         __mpz_struct tmp;
         _val_to_mpz(&tmp, args[i]);
         mpz_sub(&acc, &acc, &tmp);
         mpz_clear(&tmp);
         }
      Value r = _mpz_to_value(&acc);
      mpz_clear(&acc);
      return r;
      }
   if (args.size() == 1)
      return _wrap_numany(_numany_neg(_any_num(args[0], "-", app, 1)));
   NumAny result = _any_num(args[0], "-", app, 1);
   for (size_t i = 1; i < args.size(); ++i)
      result = _numany_sub(result, _any_num(args[i], "-", app, (int)(i + 1)));
   return _wrap_numany(result);
   }

static Value _prim_mul(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (_has_any_complex(args))
      {
      CplxResult acc{int64_t(1), int64_t(0), true, true};
      for (size_t i = 0; i < args.size(); ++i)
         {
         CplxResult a = _extract_complex(args[i]);
         if (!a.valid)
            throw SchemeTypeError("*: argument " + std::to_string(i + 1) + " is not a number", _src(app));
         if (!a.exact)
            acc.exact = false;
         NumAny new_re = _numany_sub(_numany_mul(acc.re, a.re), _numany_mul(acc.im, a.im));
         NumAny new_im = _numany_add(_numany_mul(acc.re, a.im), _numany_mul(acc.im, a.re));
         acc.re = new_re;
         acc.im = new_im;
         }
      return _wrap_cplx_result(acc.re, acc.im, acc.exact);
      }
   // Fast path: all exact integers (fixnum or bignum).
   bool all_int = true;
   for (auto& a : args)
      if (!is_integer(a) && !is_bignum(a))
         {
         all_int = false;
         break;
         }
   if (all_int)
      {
      // Try int64 first.
      bool has_bignum = false;
      for (auto& a : args)
         if (is_bignum(a))
            {
            has_bignum = true;
            break;
            }
      if (!has_bignum)
         {
         int64_t prod = 1;
         bool overflow = false;
         for (auto& a : args)
            {
            int64_t out;
            if (_i64_mul(prod, as_integer(a), out))
               {
               overflow = true;
               break;
               }
            prod = out;
            }
         if (!overflow)
            return make_integer(prod);
         }
      // mpz path.
      __mpz_struct acc;
      mpz_init_set_si(&acc, 1);
      for (auto& a : args)
         {
         __mpz_struct tmp;
         _val_to_mpz(&tmp, a);
         mpz_mul(&acc, &acc, &tmp);
         mpz_clear(&tmp);
         }
      Value result = _mpz_to_value(&acc);
      mpz_clear(&acc);
      return result;
      }
   NumAny result{int64_t(1)};
   for (size_t i = 0; i < args.size(); ++i)
      result = _numany_mul(result, _any_num(args[i], "*", app, (int)(i + 1)));
   return _wrap_numany(result);
   }

static Value _prim_div(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (_has_any_complex(args))
      {
      CplxResult acc = _extract_complex(args[0]);
      if (!acc.valid)
         throw SchemeTypeError("/: argument 1 is not a number", _src(app));
      if (args.size() == 1)
         {
         CplxResult one{int64_t(1), int64_t(0), true, true};
         acc = _complex_div(one, acc);
         if (!acc.valid)
            throw SchemeTypeError("/: division by zero", _src(app));
         return _wrap_cplx_result(acc.re, acc.im, acc.exact);
         }
      for (size_t i = 1; i < args.size(); ++i)
         {
         CplxResult b = _extract_complex(args[i]);
         if (!b.valid)
            throw SchemeTypeError("/: argument " + std::to_string(i + 1) + " is not a number", _src(app));
         acc = _complex_div(acc, b);
         if (!acc.valid)
            throw SchemeTypeError("/: division by zero", _src(app));
         }
      return _wrap_cplx_result(acc.re, acc.im, acc.exact);
      }
   if (args.size() == 1)
      {
      NumAny n = _any_num(args[0], "/", app, 1);
      if (_is_exact_zero(n))
         throw SchemeTypeError("/: division by zero", _src(app));
      try
         {
         return _wrap_numany(_exact_div_numany(NumAny{int64_t(1)}, n));
         }
      catch (std::domain_error&)
         {
         throw SchemeTypeError("/: division by zero", _src(app));
         }
      }
   NumAny result = _any_num(args[0], "/", app, 1);
   for (size_t i = 1; i < args.size(); ++i)
      {
      NumAny divisor = _any_num(args[i], "/", app, (int)(i + 1));
      if (_is_exact_zero(divisor))
         throw SchemeTypeError("/: division by zero", _src(app));
      try
         {
         result = _exact_div_numany(result, divisor);
         }
      catch (std::domain_error&)
         {
         throw SchemeTypeError("/: division by zero", _src(app));
         }
      }
   return _wrap_numany(result);
   }

static Value _prim_abs(Context* ctx, Environment* env, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (is_complex(v))
      return make_real(std::hypot(as_complex_real(v), as_complex_imag(v)));
   if (is_exact_complex(v))
      {
      double re = static_cast<double>(_as_exact_component(as_exact_complex_real(v)));
      double im = static_cast<double>(_as_exact_component(as_exact_complex_imag(v)));
      return make_real(std::hypot(re, im));
      }
   NumAny n = _num_real(v, "abs", app, 1);
   return std::visit([](auto x) -> Value
                     {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, int64_t>)
            return make_integer(x < 0 ? -x : x);
        else if constexpr (std::is_same_v<T, Rat>) {
            Rat r = rat_abs(x);
            if (r.denominator == 1) return make_integer(r.numerator);
            return make_rational(r.numerator, r.denominator);
        } else {
            return make_real(std::abs(x));
        } }, n);
   }

static Value _prim_quotient(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto n = _check_int_x(args[0], "quotient", app, 1);
   auto d = _check_int_x(args[1], "quotient", app, 2);
   if (d.re == 0)
      throw SchemeTypeError("quotient: division by zero", _src(app));
   int64_t r = _trunc_div(n.re, d.re);
   return (!n.exact || !d.exact) ? make_real((double)r) : make_integer(r);
   }

static Value _prim_remainder(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto n = _check_int_x(args[0], "remainder", app, 1);
   auto d = _check_int_x(args[1], "remainder", app, 2);
   if (d.re == 0)
      throw SchemeTypeError("remainder: division by zero", _src(app));
   int64_t r = n.re - d.re * _trunc_div(n.re, d.re);
   return (!n.exact || !d.exact) ? make_real((double)r) : make_integer(r);
   }

static Value _prim_modulo(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto n = _check_int_x(args[0], "modulo", app, 1);
   auto d = _check_int_x(args[1], "modulo", app, 2);
   if (d.re == 0)
      throw SchemeTypeError("modulo: division by zero", _src(app));
   int64_t r = n.re % d.re;
   // Adjust: result must have sign of d (Python semantics).
   if (r != 0 && ((r < 0) != (d.re < 0)))
      r += d.re;
   return (!n.exact || !d.exact) ? make_real((double)r) : make_integer(r);
   }

static Value _prim_min(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   NumAny result = _num_real(args[0], "min", app, 1);
   bool any_real = is_real(args[0]);
   for (size_t i = 1; i < args.size(); ++i)
      {
      NumAny v = _num_real(args[i], "min", app, (int)(i + 1));
      if (is_real(args[i]))
         any_real = true;
      if (std::holds_alternative<double>(v) && std::isnan(std::get<double>(v)))
         {
         result = v;
         }
      else if (!(std::holds_alternative<double>(result) && std::isnan(std::get<double>(result))))
         {
         if (_numany_lt(v, result))
            result = v;
         }
      }
   if (any_real && !std::holds_alternative<double>(result))
      result = _numany_to_double(result);
   return _wrap_numany(result);
   }

static Value _prim_max(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   NumAny result = _num_real(args[0], "max", app, 1);
   bool any_real = is_real(args[0]);
   for (size_t i = 1; i < args.size(); ++i)
      {
      NumAny v = _num_real(args[i], "max", app, (int)(i + 1));
      if (is_real(args[i]))
         any_real = true;
      if (std::holds_alternative<double>(v) && std::isnan(std::get<double>(v)))
         {
         result = v;
         }
      else if (!(std::holds_alternative<double>(result) && std::isnan(std::get<double>(result))))
         {
         if (_numany_lt(result, v))
            result = v;
         }
      }
   if (any_real && !std::holds_alternative<double>(result))
      result = _numany_to_double(result);
   return _wrap_numany(result);
   }

static Value _prim_gcd(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (args.empty())
      return make_integer(0);
   auto a0 = _check_int_x(args[0], "gcd", app, 1);
   bool any_inexact = !a0.exact;
   int64_t result = std::abs(a0.re);
   for (size_t i = 1; i < args.size(); ++i)
      {
      auto v = _check_int_x(args[i], "gcd", app, (int)(i + 1));
      if (!v.exact)
         any_inexact = true;
      result = std::gcd(result, std::abs(v.re));
      }
   return any_inexact ? make_real((double)result) : make_integer(result);
   }

static Value _prim_lcm(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (args.empty())
      return make_integer(1);
   auto a0 = _check_int_x(args[0], "lcm", app, 1);
   bool any_inexact = !a0.exact;
   int64_t result = std::abs(a0.re);
   for (size_t i = 1; i < args.size(); ++i)
      {
      auto v = _check_int_x(args[i], "lcm", app, (int)(i + 1));
      if (!v.exact)
         any_inexact = true;
      if (result == 0 || v.re == 0)
         {
         result = 0;
         }
      else
         {
         int64_t av = std::abs(v.re);
         int64_t g = std::gcd(result, av);
         result = result / g * av;
         }
      }
   return any_inexact ? make_real((double)result) : make_integer(result);
   }

static Value _prim_expt(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value &base_v = args[0], &exp_v = args[1];
   // Complex path.
   if (_is_complex_type(base_v) || _is_complex_type(exp_v))
      {
      std::complex<double> cb = _val_to_complex(base_v);
      if (is_integer(exp_v))
         {
         return _wrap_cplx(_clean_complex(_cpow_int(cb, as_integer(exp_v))));
         }
      std::complex<double> ce = _val_to_complex(exp_v);
      return _wrap_cplx(_clean_complex(std::pow(cb, ce)));
      }
   // Exact integer base + exact integer exponent: use mpz_pow_ui for bignum-safe result.
   if ((is_integer(base_v) || is_bignum(base_v)) && is_integer(exp_v))
      {
      int64_t e = as_integer(exp_v);
      if (e >= 0)
         {
         __mpz_struct base_z, result_z;
         _val_to_mpz(&base_z, base_v);
         mpz_init(&result_z);
         unsigned long exp_ul = (e > (int64_t)ULONG_MAX) ? ULONG_MAX : (unsigned long)e;
         mpz_pow_ui(&result_z, &base_z, exp_ul);
         Value v = _mpz_to_value(&result_z);
         mpz_clear(&base_z);
         mpz_clear(&result_z);
         return v;
         }
      else
         {
         // Negative exponent: result is 1/base^(-e), exact rational.
         Rat base_r(as_integer(base_v));
         int neg_e = ((-e) > (int64_t)INT_MAX) ? INT_MAX : (int)(-e);
         Rat pos = base_r.pow(neg_e);
         if (pos.numerator == 0)
            throw SchemeTypeError("expt: division by zero", _src(&exp_v));
         return _wrap_numany(NumAny{Rat(1) / pos});
         }
      }
   // Exact rational base + exact integer exponent: use Rat.pow for exact result.
   if (is_rational(base_v) && is_integer(exp_v))
      {
      int64_t e = as_integer(exp_v);
      Rat base_r(as_rational_num(base_v), as_rational_den(base_v));
      if (e >= 0)
         {
         int exp_i = (e > (int64_t)INT_MAX) ? INT_MAX : (int)e;
         return _wrap_numany(NumAny{base_r.pow(exp_i)});
         }
      else
         {
         int neg_e = ((-e) > (int64_t)INT_MAX) ? INT_MAX : (int)(-e);
         Rat pos = base_r.pow(neg_e);
         if (pos.numerator == 0)
            throw SchemeTypeError("expt: division by zero", _src(&exp_v));
         return _wrap_numany(NumAny{Rat(1) / pos});
         }
      }
   // Float path.
   double fb = _numany_to_double(_any_num(base_v, "expt", app, 1));
   double fe = _numany_to_double(_any_num(exp_v, "expt", app, 2));
   if (fb < 0.0 && std::isfinite(fb) && std::isfinite(fe) && fe != std::trunc(fe))
      {
      return _wrap_cplx(_clean_complex(
          std::pow(std::complex<double>(fb, 0.0), std::complex<double>(fe, 0.0))));
      }
   double result = std::pow(fb, fe);
   if (std::isnan(result) && !std::isnan(fb) && !std::isnan(fe))
      {
      return _wrap_cplx(_clean_complex(
          std::pow(std::complex<double>(fb, 0.0), std::complex<double>(fe, 0.0))));
      }
   return make_real(result);
   }

static Value _prim_sqrt(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (_is_complex_type(v))
      return _wrap_cplx(std::sqrt(_val_to_complex(v)));
   if (is_integer(v))
      {
      int64_t n = as_integer(v);
      if (n >= 0)
         {
         int64_t r = (int64_t)std::sqrt((double)n);
         while (r > 0 && r * r > n)
            --r;
         while ((r + 1) * (r + 1) <= n)
            ++r;
         if (r * r == n)
            return make_integer(r);
         }
      }
   if (is_rational(v))
      {
      int64_t num = as_rational_num(v), den = as_rational_den(v);
      if (num >= 0)
         {
         int64_t rn = (int64_t)std::sqrt((double)num);
         int64_t rd = (int64_t)std::sqrt((double)den);
         while (rn > 0 && rn * rn > num)
            --rn;
         while ((rn + 1) * (rn + 1) <= num)
            ++rn;
         while (rd > 0 && rd * rd > den)
            --rd;
         while ((rd + 1) * (rd + 1) <= den)
            ++rd;
         if (rn * rn == num && rd * rd == den)
            {
            Rat r(rn, rd);
            return _wrap_numany(NumAny{r});
            }
         }
      }
   // Float path.
   double fv;
   if (is_integer(v))
      fv = (double)as_integer(v);
   else if (is_rational(v))
      fv = as_rational_num(v) / (double)as_rational_den(v);
   else if (is_real(v))
      fv = as_real(v);
   else
      throw SchemeTypeError("sqrt: argument must be a number", _src(app));
   if (fv == 0.0)
      return make_real(0.0);
   if (fv < 0.0)
      return _wrap_cplx(std::sqrt(std::complex<double>(fv, 0.0)));
   return make_real(std::sqrt(fv));
   }

static Value _prim_square(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (_is_complex_type(v))
      {
      std::complex<double> c = _val_to_complex(v);
      return _wrap_cplx(c * c);
      }
   NumAny n = _any_num(v, "square", app, 1);
   return _wrap_numany(_numany_mul(n, n));
   }

static Value _prim_floor(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   NumAny v = _num_real(args[0], "floor", app, 1);
   return std::visit([](auto x) -> Value
                     {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, int64_t>) return make_integer(x);
        else if constexpr (std::is_same_v<T, Rat>)  return make_integer(rat_floor(x));
        else {
            if (!std::isfinite(x)) return make_real(x);
            return make_real(std::floor(x));
        } }, v);
   }

static Value _prim_ceiling(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   NumAny v = _num_real(args[0], "ceiling", app, 1);
   return std::visit([](auto x) -> Value
                     {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, int64_t>) return make_integer(x);
        else if constexpr (std::is_same_v<T, Rat>)  return make_integer(rat_ceil(x));
        else {
            if (!std::isfinite(x)) return make_real(x);
            double r = std::ceil(x);
            return make_real(r == 0.0 ? 0.0 : r);
        } }, v);
   }

static Value _prim_truncate(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   NumAny v = _num_real(args[0], "truncate", app, 1);
   return std::visit([](auto x) -> Value
                     {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, int64_t>) return make_integer(x);
        else if constexpr (std::is_same_v<T, Rat>)  return make_integer(rat_trunc(x));
        else {
            if (!std::isfinite(x)) return make_real(x);
            double r = std::trunc(x);
            return make_real(r == 0.0 ? 0.0 : r);
        } }, v);
   }

static Value _prim_round(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   NumAny v = _num_real(args[0], "round", app, 1);
   return std::visit([](auto x) -> Value
                     {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, int64_t>) return make_integer(x);
        else if constexpr (std::is_same_v<T, Rat>)  return make_integer(rat_round(x));
        else {
            if (!std::isfinite(x)) return make_real(x);
            return make_real(_banker_round(x));
        } }, v);
   }

static Value _prim_exact_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   const Value& v = args[0];
   return make_boolean(is_integer(v) || is_bignum(v) || is_rational(v) || is_exact_complex(v));
   }

static Value _prim_inexact_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   const Value& v = args[0];
   return make_boolean(is_real(v) || is_complex(v));
   }

static Value _prim_exact(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (is_integer(v) || is_bignum(v) || is_rational(v) || is_exact_complex(v))
      return v;
   if (is_real(v))
      return _float_to_exact(as_real(v), "exact", app);
   if (is_complex(v))
      {
      Value re = _float_to_exact(as_complex_real(v), "exact", app);
      double im_f = as_complex_imag(v);
      if (im_f == 0.0)
         return re;
      Value im = _float_to_exact(im_f, "exact", app);
      return make_exact_complex(re, im);
      }
   throw SchemeTypeError("exact: argument must be a number", _src(app));
   }

static Value _prim_inexact(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (is_real(v) || is_complex(v))
      return v;
   if (is_integer(v))
      return make_real((double)as_integer(v));
   if (is_bignum(v))
      return make_real(mpz_get_d(as_bignum(v)));
   if (is_rational(v))
      return make_real(as_rational_num(v) / (double)as_rational_den(v));
   if (is_exact_complex(v))
      {
      double re = static_cast<double>(_as_exact_component(as_exact_complex_real(v)));
      double im = static_cast<double>(_as_exact_component(as_exact_complex_imag(v)));
      return make_complex(re, im);
      }
   throw SchemeTypeError("inexact: argument must be a number", _src(app));
   }

static Value _prim_exact_integer_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_integer(args[0]) || is_bignum(args[0]));
   }

static Value _prim_numerator(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (is_integer(v) || is_bignum(v))
      return v;
   if (is_rational(v))
      return make_integer(as_rational_num(v));
   if (is_real(v))
      {
      double f = as_real(v);
      if (!std::isfinite(f))
         return make_real(f);
      if (f == std::trunc(f))
         return make_real(f);
      return make_real((double)Rat::from_float(f).numerator);
      }
   throw SchemeTypeError("numerator: argument must be a real number", _src(app));
   }

static Value _prim_denominator(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (is_integer(v) || is_bignum(v))
      return make_integer(1);
   if (is_rational(v))
      return make_integer(as_rational_den(v));
   if (is_real(v))
      {
      double f = as_real(v);
      if (!std::isfinite(f))
         return make_real(1.0);
      if (f == std::trunc(f))
         return make_real(1.0);
      return make_real((double)Rat::from_float(f).denominator);
      }
   throw SchemeTypeError("denominator: argument must be a real number", _src(app));
   }

static Value _prim_number_to_string(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   int radix = 10;
   if (args.size() >= 2)
      {
      if (!is_integer(args[1]))
         throw SchemeTypeError("number->string: radix must be an integer", _src(app));
      radix = (int)as_integer(args[1]);
      }
   if (is_integer(v))
      {
      int64_t n = as_integer(v);
      if (radix == 10)
         return make_string(std::to_string(n));
      if (radix == 2 || radix == 8 || radix == 16)
         return make_string(_int_to_string(n, radix));
      throw SchemeTypeError("number->string: radix must be 2, 8, 10, or 16", _src(app));
      }
   if (is_bignum(v))
      {
      if (radix != 2 && radix != 8 && radix != 10 && radix != 16)
         throw SchemeTypeError("number->string: radix must be 2, 8, 10, or 16", _src(app));
      return make_string(bignum_to_string(v, radix));
      }
   if (is_real(v))
      {
      if (radix != 10)
         throw SchemeTypeError("number->string: only radix 10 is supported for inexact numbers", _src(app));
      return make_string(_format_real(as_real(v)));
      }
   if (is_rational(v))
      {
      if (radix != 10)
         throw SchemeTypeError("number->string: only radix 10 supported for rational numbers", _src(app));
      return make_string(std::to_string(as_rational_num(v)) + "/" + std::to_string(as_rational_den(v)));
      }
   if (is_complex(v))
      {
      if (radix != 10)
         throw SchemeTypeError("number->string: only radix 10 supported for complex numbers", _src(app));
      double re = as_complex_real(v), im = as_complex_imag(v);
      std::string re_s = _format_real(re), im_s = _format_real(im);
      if (std::isnan(im) || im >= 0)
         return make_string(re_s + "+" + im_s + "i");
      return make_string(re_s + im_s + "i");
      }
   if (is_exact_complex(v))
      {
      if (radix != 10)
         throw SchemeTypeError("number->string: only radix 10 supported for complex numbers", _src(app));
      Value re_v = as_exact_complex_real(v), im_v = as_exact_complex_imag(v);
      std::string re_s = is_integer(re_v) ? std::to_string(as_integer(re_v))
                                          : std::to_string(as_rational_num(re_v)) + "/" + std::to_string(as_rational_den(re_v));
      std::string im_s = is_integer(im_v) ? std::to_string(as_integer(im_v))
                                          : std::to_string(as_rational_num(im_v)) + "/" + std::to_string(as_rational_den(im_v));
      Rat im_r = _as_exact_component(im_v);
      if (im_r >= int64_t(0))
         return make_string(re_s + "+" + im_s + "i");
      return make_string(re_s + im_s + "i");
      }
   throw SchemeTypeError("number->string: argument must be a number", _src(app));
   }

static Value _prim_string_to_number(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("string->number: first argument must be a string", _src(app));
   std::string s = as_string(args[0]);
   // strip leading/trailing whitespace
   while (!s.empty() && std::isspace((unsigned char)s.front()))
      s.erase(s.begin());
   while (!s.empty() && std::isspace((unsigned char)s.back()))
      s.pop_back();
   int radix = 10;
   if (args.size() >= 2)
      {
      if (!is_integer(args[1]))
         throw SchemeTypeError("string->number: radix must be an integer", _src(app));
      radix = (int)as_integer(args[1]);
      }
   bool explicit_radix = false;
   int exact = -1; // -1=unspecified, 1=exact, 0=inexact
   while (s.size() >= 2 && s[0] == '#')
      {
      char ch = (char)std::tolower((unsigned char)s[1]);
      if (ch == 'b')
         {
         if (explicit_radix)
            return make_boolean(false);
         radix = 2;
         explicit_radix = true;
         s = s.substr(2);
         }
      else if (ch == 'o')
         {
         if (explicit_radix)
            return make_boolean(false);
         radix = 8;
         explicit_radix = true;
         s = s.substr(2);
         }
      else if (ch == 'd')
         {
         if (explicit_radix)
            return make_boolean(false);
         radix = 10;
         explicit_radix = true;
         s = s.substr(2);
         }
      else if (ch == 'x')
         {
         if (explicit_radix)
            return make_boolean(false);
         radix = 16;
         explicit_radix = true;
         s = s.substr(2);
         }
      else if (ch == 'e')
         {
         if (exact != -1)
            return make_boolean(false);
         exact = 1;
         s = s.substr(2);
         }
      else if (ch == 'i')
         {
         if (exact != -1)
            return make_boolean(false);
         exact = 0;
         s = s.substr(2);
         }
      else
         {
         return make_boolean(false);
         }
      }
   if (s.empty())
      return make_boolean(false);
   // Try polar literal (radix 10, contains '@').
   if (radix == 10 && s.find('@') != std::string::npos)
      {
      size_t at = s.find('@');
      std::string r_str = s.substr(0, at), theta_str = s.substr(at + 1);
      if (!r_str.empty() && !theta_str.empty())
         {
         double r_val, theta_val;
         if (_parse_real_stn(r_str, r_val) && _parse_real_stn(theta_str, theta_val))
            {
            if (exact == 1)
               return make_boolean(false);
            return make_complex(r_val * std::cos(theta_val), r_val * std::sin(theta_val));
            }
         }
      }
   // Try complex literal (radix 10, ends in 'i').
   if (radix == 10 && s.size() >= 2 && s.back() == 'i')
      {
      double re_f, im_f;
      if (_parse_complex_stn(s, re_f, im_f))
         {
         if (exact == 1)
            {
            Value re_v = _float_to_exact(re_f, "string->number", nullptr);
            Value im_v = _float_to_exact(im_f, "string->number", nullptr);
            if (is_integer(im_v) && as_integer(im_v) == 0)
               return re_v;
            return make_exact_complex(re_v, im_v);
            }
         return make_complex(re_f, im_f);
         }
      }
   // Try integer.
   try
      {
      size_t pos;
      int64_t n = std::stoll(s, &pos, radix);
      if (pos == s.size())
         {
         if (exact == 0)
            return make_real((double)n);
         return make_integer(n);
         }
      }
   catch (...)
      {
      }
   // Try rational n/d.
   if (s.find('/') != std::string::npos)
      {
      size_t slash = s.find('/');
      std::string ns = s.substr(0, slash), ds = s.substr(slash + 1);
      try
         {
         int64_t num = std::stoll(ns, nullptr, radix);
         int64_t den = std::stoll(ds, nullptr, radix);
         if (den != 0)
            {
            Rat frac(num, den);
            if (exact == 0)
               return make_real(static_cast<double>(frac));
            if (frac.denominator == 1)
               return make_integer(frac.numerator);
            return make_rational(frac.numerator, frac.denominator);
            }
         }
      catch (...)
         {
         }
      }
   // Try real (radix 10 only).
   if (radix == 10)
      {
      double f;
      if (_parse_real_stn(s, f))
         {
         if (exact == 1)
            {
            if (!std::isfinite(f))
               return make_boolean(false);
            return _float_to_exact(f, "string->number", nullptr);
            }
         return make_real(f);
         }
      }
   return make_boolean(false);
   }

static Value _prim_floor_quotient(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto n = _check_int_x(args[0], "floor-quotient", app, 1);
   auto d = _check_int_x(args[1], "floor-quotient", app, 2);
   if (d.re == 0)
      throw SchemeTypeError("floor-quotient: divide by zero", _src(app));
   int64_t r = n.re / d.re;
   // Adjust toward -inf for floor division.
   if ((n.re ^ d.re) < 0 && r * d.re != n.re)
      --r;
   return (!n.exact || !d.exact) ? make_real((double)r) : make_integer(r);
   }

static Value _prim_floor_remainder(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto n = _check_int_x(args[0], "floor-remainder", app, 1);
   auto d = _check_int_x(args[1], "floor-remainder", app, 2);
   if (d.re == 0)
      throw SchemeTypeError("floor-remainder: divide by zero", _src(app));
   int64_t r = n.re % d.re;
   if (r != 0 && ((r < 0) != (d.re < 0)))
      r += d.re;
   return (!n.exact || !d.exact) ? make_real((double)r) : make_integer(r);
   }

static Value _prim_floor_div(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto n = _check_int_x(args[0], "floor/", app, 1);
   auto d = _check_int_x(args[1], "floor/", app, 2);
   if (d.re == 0)
      throw SchemeTypeError("floor/: divide by zero", _src(app));
   int64_t q = n.re / d.re;
   if ((n.re ^ d.re) < 0 && q * d.re != n.re)
      --q;
   int64_t r = n.re - q * d.re;
   bool inexact = !n.exact || !d.exact;
   return make_multi_values({inexact ? make_real((double)q) : make_integer(q),
                             inexact ? make_real((double)r) : make_integer(r)});
   }

static Value _prim_truncate_quotient(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto n = _check_int_x(args[0], "truncate-quotient", app, 1);
   auto d = _check_int_x(args[1], "truncate-quotient", app, 2);
   if (d.re == 0)
      throw SchemeTypeError("truncate-quotient: divide by zero", _src(app));
   int64_t r = _trunc_div(n.re, d.re);
   return (!n.exact || !d.exact) ? make_real((double)r) : make_integer(r);
   }

static Value _prim_truncate_remainder(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto n = _check_int_x(args[0], "truncate-remainder", app, 1);
   auto d = _check_int_x(args[1], "truncate-remainder", app, 2);
   if (d.re == 0)
      throw SchemeTypeError("truncate-remainder: divide by zero", _src(app));
   int64_t r = n.re - _trunc_div(n.re, d.re) * d.re;
   return (!n.exact || !d.exact) ? make_real((double)r) : make_integer(r);
   }

static Value _prim_truncate_div(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto n = _check_int_x(args[0], "truncate/", app, 1);
   auto d = _check_int_x(args[1], "truncate/", app, 2);
   if (d.re == 0)
      throw SchemeTypeError("truncate/: divide by zero", _src(app));
   int64_t q = _trunc_div(n.re, d.re);
   int64_t r = n.re - q * d.re;
   bool inexact = !n.exact || !d.exact;
   return make_multi_values({inexact ? make_real((double)q) : make_integer(q),
                             inexact ? make_real((double)r) : make_integer(r)});
   }

static Value _prim_exact_integer_sqrt(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   int64_t n = _check_int(args[0], "exact-integer-sqrt", app, 1);
   if (n < 0)
      throw SchemeTypeError("exact-integer-sqrt: argument must be non-negative", _src(app));
   int64_t r = (int64_t)std::sqrt((double)n);
   while (r > 0 && r * r > n)
      --r;
   while ((r + 1) * (r + 1) <= n)
      ++r;
   return make_multi_values({make_integer(r), make_integer(n - r * r)});
   }

static Value _prim_features(Context*, Environment*, std::vector<Value>&, const Value*)
   {
   // Port of arithmetic.py _prim_features: mirrors _FEATURES insertion order.
   // Must match PyScheme's output exactly so the shared test log passes.
   static const char* feats[] = {
       "r7rs", "exact-closed", "exact-rational", "ratios",
       "ieee-float", "full-unicode", "pyscheme",
#ifdef _WIN32
       "windows",
#elif defined(__linux__)
       "posix", "linux",
#elif defined(__APPLE__)
       "posix", "darwin",
#else
       "posix",
#endif
       nullptr};
   Value result = NIL_VALUE;
   int n = 0;
   while (feats[n])
      ++n;
   for (int i = n - 1; i >= 0; --i)
      result = alloc_cons(make_symbol(feats[i], nullptr), result, nullptr);
   return result;
   }

static Value _prim_exp(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (_is_complex_type(v))
      return _wrap_cplx(std::exp(_val_to_complex(v)));
   double f = _numany_to_double(_any_num(v, "exp", app, 1));
   double result = std::exp(f);
   return make_real(std::isinf(result) ? std::numeric_limits<double>::infinity() : result);
   }

static Value _prim_log(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (_is_complex_type(v))
      {
      std::complex<double> result = std::log(_val_to_complex(v));
      if (args.size() >= 2)
         {
         std::complex<double> b = _is_complex_type(args[1])
                                      ? _val_to_complex(args[1])
                                      : std::complex<double>(_numany_to_double(_any_num(args[1], "log", app, 2)), 0.0);
         result = result / std::log(b);
         }
      return _wrap_cplx(result);
      }
   double f = _numany_to_double(_any_num(v, "log", app, 1));
   if (f == 0.0)
      {
      double r = -std::numeric_limits<double>::infinity();
      if (args.size() >= 2)
         {
         double bf = _numany_to_double(_any_num(args[1], "log", app, 2));
         r = (bf <= 0.0) ? std::numeric_limits<double>::quiet_NaN() : r / std::log(bf);
         }
      return make_real(r);
      }
   if (f < 0.0)
      {
      std::complex<double> result = std::log(std::complex<double>(f, 0.0));
      if (args.size() >= 2)
         {
         std::complex<double> b = _is_complex_type(args[1])
                                      ? _val_to_complex(args[1])
                                      : std::complex<double>(_numany_to_double(_any_num(args[1], "log", app, 2)), 0.0);
         result = result / std::log(b);
         }
      return _wrap_cplx(result);
      }
   double r = std::log(f);
   if (args.size() >= 2)
      {
      double bf = _numany_to_double(_any_num(args[1], "log", app, 2));
      r = (bf == 0.0 || bf < 0.0) ? std::numeric_limits<double>::quiet_NaN() : r / std::log(bf);
      }
   return make_real(r);
   }

static Value _prim_sin(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (_is_complex_type(v))
      return _wrap_cplx(std::sin(_val_to_complex(v)));
   return make_real(std::sin(_numany_to_double(_any_num(v, "sin", app, 1))));
   }

static Value _prim_cos(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (_is_complex_type(v))
      return _wrap_cplx(std::cos(_val_to_complex(v)));
   return make_real(std::cos(_numany_to_double(_any_num(v, "cos", app, 1))));
   }

static Value _prim_tan(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (_is_complex_type(v))
      return _wrap_cplx(std::tan(_val_to_complex(v)));
   return make_real(std::tan(_numany_to_double(_any_num(v, "tan", app, 1))));
   }

static Value _prim_asin(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (_is_complex_type(v))
      return _wrap_cplx(std::asin(_val_to_complex(v)));
   double f = _numany_to_double(_any_num(v, "asin", app, 1));
   if (f < -1.0 || f > 1.0)
      return _wrap_cplx(std::asin(std::complex<double>(f, 0.0)));
   return make_real(std::asin(f));
   }

static Value _prim_acos(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (_is_complex_type(v))
      return _wrap_cplx(std::acos(_val_to_complex(v)));
   double f = _numany_to_double(_any_num(v, "acos", app, 1));
   if (f < -1.0 || f > 1.0)
      return _wrap_cplx(std::acos(std::complex<double>(f, 0.0)));
   return make_real(std::acos(f));
   }

static Value _prim_atan(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (_is_complex_type(v))
      {
      if (args.size() >= 2)
         throw SchemeTypeError("atan: two-argument form requires real numbers", _src(app));
      return _wrap_cplx(std::atan(_val_to_complex(v)));
      }
   double f = _numany_to_double(_any_num(v, "atan", app, 1));
   if (args.size() >= 2)
      {
      double f2 = _numany_to_double(_any_num(args[1], "atan", app, 2));
      return make_real(std::atan2(f, f2));
      }
   return make_real(std::atan(f));
   }

static Value _prim_make_rectangular(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value &re = args[0], &im = args[1];
   bool re_exact = is_integer(re) || is_rational(re);
   bool im_exact = is_integer(im) || is_rational(im);
   if (re_exact && im_exact)
      {
      Rat im_r = _as_exact_component(im);
      if (im_r == int64_t(0))
         return re;
      return make_exact_complex(re, im);
      }
   double re_f, im_f;
   if (is_integer(re))
      re_f = (double)as_integer(re);
   else if (is_rational(re))
      re_f = as_rational_num(re) / (double)as_rational_den(re);
   else if (is_real(re))
      re_f = as_real(re);
   else
      throw SchemeTypeError("make-rectangular: argument 1 is not a real number", _src(app));
   if (is_integer(im))
      im_f = (double)as_integer(im);
   else if (is_rational(im))
      im_f = as_rational_num(im) / (double)as_rational_den(im);
   else if (is_real(im))
      im_f = as_real(im);
   else
      throw SchemeTypeError("make-rectangular: argument 2 is not a real number", _src(app));
   return make_complex(re_f, im_f);
   }

static Value _prim_make_polar(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   double r = _numany_to_double(_num_real(args[0], "make-polar", app, 1));
   double theta = _numany_to_double(_num_real(args[1], "make-polar", app, 2));
   return make_complex(r * std::cos(theta), r * std::sin(theta));
   }

static Value _prim_real_part(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (is_exact_complex(v))
      return as_exact_complex_real(v);
   if (is_complex(v))
      return make_real(as_complex_real(v));
   if (is_integer(v) || is_bignum(v) || is_rational(v) || is_real(v))
      return v;
   throw SchemeTypeError("real-part: argument must be a number", _src(app));
   }

static Value _prim_imag_part(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (is_exact_complex(v))
      return as_exact_complex_imag(v);
   if (is_complex(v))
      return make_real(as_complex_imag(v));
   if (is_integer(v) || is_bignum(v) || is_rational(v))
      return make_integer(0);
   if (is_real(v))
      return make_real(0.0);
   throw SchemeTypeError("imag-part: argument must be a number", _src(app));
   }

static Value _prim_magnitude(Context* ctx, Environment* env, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (is_exact_complex(v))
      {
      double re = static_cast<double>(_as_exact_component(as_exact_complex_real(v)));
      double im = static_cast<double>(_as_exact_component(as_exact_complex_imag(v)));
      return make_real(std::hypot(re, im));
      }
   if (is_complex(v))
      return make_real(std::hypot(as_complex_real(v), as_complex_imag(v)));
   return _prim_abs(ctx, env, args, app);
   }

static Value _prim_angle(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (is_exact_complex(v))
      {
      double re = static_cast<double>(_as_exact_component(as_exact_complex_real(v)));
      double im = static_cast<double>(_as_exact_component(as_exact_complex_imag(v)));
      return make_real(std::atan2(im, re));
      }
   if (is_complex(v))
      return make_real(std::atan2(as_complex_imag(v), as_complex_real(v)));
   NumAny n = _num_real(v, "angle", app, 1);
   if (std::holds_alternative<double>(n) && std::isnan(std::get<double>(n)))
      return make_real(std::numeric_limits<double>::quiet_NaN());
   if (_numany_lt(NumAny{int64_t(0)}, n) || _numany_is_zero(n))
      return make_real(0.0);
   return make_real(std::acos(-1.0));
   }

static Value _prim_rationalize(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   NumAny x = _num_real(args[0], "rationalize", app, 1);
   NumAny delta = _num_real(args[1], "rationalize", app, 2);
   bool inexact = std::holds_alternative<double>(x) || std::holds_alternative<double>(delta);
   if (auto* fp = std::get_if<double>(&x))
      {
      if (!std::isfinite(*fp))
         return make_real(*fp);
      }
   if (auto* fp = std::get_if<double>(&delta))
      {
      if (std::isinf(*fp))
         return inexact ? make_real(0.0) : make_integer(0);
      if (std::isnan(*fp))
         return make_real(std::numeric_limits<double>::quiet_NaN());
      }
   // When either arg is inexact, compute lo/hi bounds in double precision first
   // to avoid int64 overflow in Rat arithmetic of large-denominator floats
   // (e.g. 0.3 has denominator 2^54; subtracting two such rationals overflows).
   if (inexact)
      {
      double xd = std::visit([](auto v) -> double
                             { return static_cast<double>(v); }, x);
      double dd = std::visit([](auto v) -> double
                             { return static_cast<double>(v); }, delta);
      if (dd < 0.0)
         dd = -dd;
      double lo_d = xd - dd, hi_d = xd + dd;
      Rat rlo = Rat::from_float(lo_d), rhi = Rat::from_float(hi_d);
      Rat r;
      if (rlo <= Rat(0) && rhi >= Rat(0))
         r = Rat(0);
      else if (rlo > Rat(0))
         r = _simplest_rational(rlo, rhi);
      else
         r = -_simplest_rational(-rhi, -rlo);
      return make_real(static_cast<double>(r));
      }
   Rat fx = _numany_to_rat(x);
   Rat fd = _numany_to_rat(delta);
   if (fd < Rat(0))
      fd = -fd;
   Rat r = _rationalize_exact(fx, fd);
   return _wrap_numany(NumAny{r});
   }

static Value _prim_random(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   static std::mt19937_64 rng(std::random_device{}());
   const Value& n = args[0];
   if (is_integer(n))
      {
      int64_t v = as_integer(n);
      if (v <= 0)
         throw SchemeTypeError("random: argument must be positive", _src(app));
      std::uniform_int_distribution<int64_t> dist(0, v - 1);
      return make_integer(dist(rng));
      }
   if (is_real(n))
      {
      double v = as_real(n);
      if (v <= 0.0)
         throw SchemeTypeError("random: argument must be positive", _src(app));
      std::uniform_real_distribution<double> dist(0.0, v);
      return make_real(dist(rng));
      }
   throw SchemeTypeError("random: argument must be a number", _src(app));
   }

// ── Registration ─────────────────────────────────────────────────────────────

void register_arithmetic()
   {
   register_primitive("+", 0, -1, _prim_add, "",
                      "Return the sum of the arguments.  With no arguments, returns 0.  R7RS 6.2.6.", CATEGORY);
   register_primitive("-", 1, -1, _prim_sub, "",
                      "With one argument, return its additive inverse.  With multiple arguments, return the first minus the rest.  R7RS 6.2.6.", CATEGORY);
   register_primitive("*", 0, -1, _prim_mul, "",
                      "Return the product of the arguments.  With no arguments, returns 1.  R7RS 6.2.6.", CATEGORY);
   register_primitive("/", 1, -1, _prim_div, "",
                      "With one argument, return its reciprocal.  With multiple arguments, return the first divided by the product of the rest.  R7RS 6.2.6.", CATEGORY);
   register_primitive("abs", 1, 1, _prim_abs, "", "Return the absolute value of a.  R7RS 6.2.6.", CATEGORY);
   register_primitive("quotient", 2, 2, _prim_quotient, "", "Integer division of a by b truncated toward zero.  R7RS 6.2.6.", CATEGORY);
   register_primitive("remainder", 2, 2, _prim_remainder, "", "Integer remainder with the sign of the dividend.  R7RS 6.2.6.", CATEGORY);
   register_primitive("modulo", 2, 2, _prim_modulo, "", "Integer modulo with the sign of the divisor.  R7RS 6.2.6.", CATEGORY);
   register_primitive("min", 1, -1, _prim_min, "", "Return the smallest of its numeric arguments.  R7RS 6.2.6.", CATEGORY);
   register_primitive("max", 1, -1, _prim_max, "", "Return the largest of its numeric arguments.  R7RS 6.2.6.", CATEGORY);
   register_primitive("gcd", 0, -1, _prim_gcd, "", "Return the greatest common divisor of the integer arguments.  R7RS 6.2.6.", CATEGORY);
   register_primitive("lcm", 0, -1, _prim_lcm, "", "Return the least common multiple of the integer arguments.  R7RS 6.2.6.", CATEGORY);
   register_primitive("expt", 2, 2, _prim_expt, "", "Return base raised to the exponent.  R7RS 6.2.6.", CATEGORY);
   register_primitive("sqrt", 1, 1, _prim_sqrt, "", "Return the principal square root; exact integer when argument is a perfect square.  R7RS 6.2.6.", CATEGORY);
   register_primitive("square", 1, 1, _prim_square, "", "Return (* x x).  R7RS 6.2.6.", CATEGORY);
   register_primitive("floor", 1, 1, _prim_floor, "", "Return the largest integer not greater than x.  R7RS 6.2.6.", CATEGORY);
   register_primitive("ceiling", 1, 1, _prim_ceiling, "", "Return the smallest integer not less than x.  R7RS 6.2.6.", CATEGORY);
   register_primitive("truncate", 1, 1, _prim_truncate, "", "Return the integer part of x (toward zero).  R7RS 6.2.6.", CATEGORY);
   register_primitive("round", 1, 1, _prim_round, "", "Return the nearest integer; ties round to even (banker's rounding).  R7RS 6.2.6.", CATEGORY);
   register_primitive("exact?", 1, 1, _prim_exact_p, "", "Return #t if obj is an exact number.  R7RS 6.2.6.", CATEGORY);
   register_primitive("inexact?", 1, 1, _prim_inexact_p, "", "Return #t if obj is an inexact number.  R7RS 6.2.6.", CATEGORY);
   register_primitive("exact", 1, 1, _prim_exact, "", "Convert a number to its exact form.  R7RS 6.2.6.", CATEGORY);
   register_primitive("inexact", 1, 1, _prim_inexact, "", "Convert a number to its inexact form.  R7RS 6.2.6.", CATEGORY);
   register_primitive("exact-integer?", 1, 1, _prim_exact_integer_p, "", "Return #t if obj is an exact integer.  R7RS 6.2.6.", CATEGORY);
   register_primitive("numerator", 1, 1, _prim_numerator, "", "Return the numerator of a rational number.  R7RS 6.2.6.", CATEGORY);
   register_primitive("denominator", 1, 1, _prim_denominator, "", "Return the denominator of a rational number.  R7RS 6.2.6.", CATEGORY);
   register_primitive("number->string", 1, 2, _prim_number_to_string, "",
                      "(number->string number [radix]) returns a string representation.  Radix may be 2, 8, 10, or 16.  R7RS 6.2.6.", CATEGORY);
   register_primitive("string->number", 1, 2, _prim_string_to_number, "",
                      "(string->number string [radix]) parses a string; returns the number on success, #f on failure.  R7RS 6.2.6.", CATEGORY);
   register_primitive("floor-quotient", 2, 2, _prim_floor_quotient, "", "Integer floor division; result has the sign of the divisor.  R7RS 6.2.6.", CATEGORY);
   register_primitive("floor-remainder", 2, 2, _prim_floor_remainder, "", "Integer floor remainder; result has the sign of the divisor.  R7RS 6.2.6.", CATEGORY);
   register_primitive("floor/", 2, 2, _prim_floor_div, "",
                      "(floor/ n d) returns two values: the floor quotient and remainder.  R7RS 6.2.6.", CATEGORY);
   register_primitive("truncate-quotient", 2, 2, _prim_truncate_quotient, "", "Integer truncate division (toward zero).  R7RS 6.2.6.", CATEGORY);
   register_primitive("truncate-remainder", 2, 2, _prim_truncate_remainder, "", "Integer truncate remainder.  R7RS 6.2.6.", CATEGORY);
   register_primitive("truncate/", 2, 2, _prim_truncate_div, "",
                      "(truncate/ n d) returns two values: the truncate quotient and remainder.  R7RS 6.2.6.", CATEGORY);
   register_primitive("exact-integer-sqrt", 1, 1, _prim_exact_integer_sqrt, "",
                      "(exact-integer-sqrt n) returns two values: floor(sqrt(n)) and n - r*r.  R7RS 6.2.6.", CATEGORY);
   register_primitive("features", 0, 0, _prim_features, "",
                      "Return a list of feature identifiers supported by this implementation.  R7RS 5.6.2.", CATEGORY);
   register_primitive("exp", 1, 1, _prim_exp, "", "Return e^z.  R7RS 6.2.6.", CATEGORY);
   register_primitive("log", 1, 2, _prim_log, "",
                      "(log z) returns ln(z).  (log z w) returns log base w.  R7RS 6.2.6.", CATEGORY);
   register_primitive("sin", 1, 1, _prim_sin, "", "Return the sine of z (radians).  R7RS 6.2.6.", CATEGORY);
   register_primitive("cos", 1, 1, _prim_cos, "", "Return the cosine of z (radians).  R7RS 6.2.6.", CATEGORY);
   register_primitive("tan", 1, 1, _prim_tan, "", "Return the tangent of z (radians).  R7RS 6.2.6.", CATEGORY);
   register_primitive("asin", 1, 1, _prim_asin, "", "Return the arcsine of z in radians.  R7RS 6.2.6.", CATEGORY);
   register_primitive("acos", 1, 1, _prim_acos, "", "Return the arccosine of z in radians.  R7RS 6.2.6.", CATEGORY);
   register_primitive("atan", 1, 2, _prim_atan, "",
                      "(atan z) returns the arctangent.  (atan y x) returns atan2(y,x).  R7RS 6.2.6.", CATEGORY);
   register_primitive("make-rectangular", 2, 2, _prim_make_rectangular, "",
                      "(make-rectangular x y) constructs x+yi.  R7RS 6.2.6.", CATEGORY);
   register_primitive("make-polar", 2, 2, _prim_make_polar, "",
                      "(make-polar r theta) constructs a complex from polar form.  R7RS 6.2.6.", CATEGORY);
   register_primitive("real-part", 1, 1, _prim_real_part, "", "Return the real part of z.  R7RS 6.2.6.", CATEGORY);
   register_primitive("imag-part", 1, 1, _prim_imag_part, "", "Return the imaginary part of z.  R7RS 6.2.6.", CATEGORY);
   register_primitive("magnitude", 1, 1, _prim_magnitude, "", "Return |z|.  R7RS 6.2.6.", CATEGORY);
   register_primitive("angle", 1, 1, _prim_angle, "",
                      "Return the angle of z in radians.  For positive real: 0.0; negative: pi.  R7RS 6.2.6.", CATEGORY);
   register_primitive("rationalize", 2, 2, _prim_rationalize, "",
                      "(rationalize x delta) returns the simplest rational y such that |x-y| <= delta.  R7RS 6.2.6.", CATEGORY);
   register_primitive("exact->inexact", 1, 1, _prim_inexact, "", "Alias for inexact.  R6RS/R7RS.", CATEGORY);
   register_primitive("inexact->exact", 1, 1, _prim_exact, "", "Alias for exact.  R6RS/R7RS.", CATEGORY);
   register_primitive("random", 1, 1, _prim_random, "",
                      "(random n) returns a random integer in [0,n) or real in [0.0,n).  MIT Scheme / SICP.", CATEGORY);
   }
