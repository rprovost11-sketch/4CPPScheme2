// rational.cpp -- Exact rational arithmetic type.
// Direct port of pyscheme/rational.py _Rat.
#include "rational.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <numeric>
#include <string>

// ── Internal helpers ──────────────────────────────────────────────────────────

static int64_t rat_gcd(int64_t a, int64_t b)
   {
   // std::gcd (C++17) returns non-negative, handles sign
   return std::gcd(a < 0 ? -a : a, b);
   }

// ── Rat constructors ──────────────────────────────────────────────────────────

Rat::Rat() : numerator(0), denominator(1) {}

Rat::Rat(int64_t num, int64_t den)
   {
   if (den == 0)
      throw std::domain_error("rational denominator is zero");
   if (den < 0)
      {
      num = -num;
      den = -den;
      }
   int64_t g = rat_gcd(num < 0 ? -num : num, den);
   numerator = num / g;
   denominator = den / g;
   }

// Port of _Rat.from_float: decompose double as sig * 2^e.
// May lose precision for extreme values (subnormals, very large doubles)
// since both fields are int64_t rather than arbitrary-precision Python ints.
Rat Rat::from_float(double f)
   {
   if (f == 0.0)
      return Rat(0, 1);
   int bexp;
   double mant = std::frexp(f, &bexp);
   // f = mant * 2^bexp, |mant| in [0.5, 1)
   // sig = mant * 2^53 is exact 53-bit integer
   int64_t sig = static_cast<int64_t>(std::ldexp(std::abs(mant), 53));
   if (f < 0.0)
      sig = -sig;
   int e = bexp - 53; // f = sig * 2^e
   if (e >= 0)
      {
      int shift = (e < 62) ? e : 62;
      return Rat(sig << shift, 1);
      }
   else
      {
      int ne = -e;
      if (ne > 62)
         ne = 62; // cap denominator to avoid UB (1LL << 63 is UB)
      return Rat(sig, int64_t(1) << ne);
      }
   }

// ── Conversions ───────────────────────────────────────────────────────────────

Rat::operator double() const
   {
   return static_cast<double>(numerator) / denominator;
   }
Rat::operator int64_t() const
   {
   return numerator / denominator;
   } // truncate toward zero
Rat::operator bool() const
   {
   return numerator != 0;
   }

// ── Unary ─────────────────────────────────────────────────────────────────────

Rat Rat::operator-() const
   {
   return Rat(-numerator, denominator);
   }
Rat Rat::operator+() const
   {
   return *this;
   }

// ── Binary: Rat op Rat ────────────────────────────────────────────────────────

Rat Rat::operator+(const Rat& o) const
   {
   return Rat(numerator * o.denominator + o.numerator * denominator,
              denominator * o.denominator);
   }
Rat Rat::operator-(const Rat& o) const
   {
   return Rat(numerator * o.denominator - o.numerator * denominator,
              denominator * o.denominator);
   }
Rat Rat::operator*(const Rat& o) const
   {
   return Rat(numerator * o.numerator, denominator * o.denominator);
   }
Rat Rat::operator/(const Rat& o) const
   {
   return Rat(numerator * o.denominator, denominator * o.numerator);
   }

// ── Binary: Rat op int64_t ────────────────────────────────────────────────────

Rat Rat::operator+(int64_t n) const
   {
   return Rat(numerator + n * denominator, denominator);
   }
Rat Rat::operator-(int64_t n) const
   {
   return Rat(numerator - n * denominator, denominator);
   }
Rat Rat::operator*(int64_t n) const
   {
   return Rat(numerator * n, denominator);
   }
Rat Rat::operator/(int64_t n) const
   {
   return Rat(numerator, denominator * n);
   }

// ── Binary: Rat op double ─────────────────────────────────────────────────────

double Rat::operator+(double f) const
   {
   return static_cast<double>(*this) + f;
   }
double Rat::operator-(double f) const
   {
   return static_cast<double>(*this) - f;
   }
double Rat::operator*(double f) const
   {
   return static_cast<double>(*this) * f;
   }
double Rat::operator/(double f) const
   {
   return static_cast<double>(*this) / f;
   }

// ── Power ─────────────────────────────────────────────────────────────────────

Rat Rat::pow(int exp) const
   {
   if (exp == 0)
      return Rat(1);
   if (exp > 0)
      {
      int64_t n = 1, d = 1;
      for (int k = 0; k < exp; ++k)
         {
         n *= numerator;
         d *= denominator;
         }
      return Rat(n, d);
      }
   // Negative exponent: invert, then raise to positive power
   int pos = -exp;
   int64_t n = 1, d = 1;
   for (int k = 0; k < pos; ++k)
      {
      n *= denominator;
      d *= numerator;
      }
   return Rat(n, d);
   }

// ── Comparisons: Rat vs Rat ───────────────────────────────────────────────────

bool Rat::operator==(const Rat& o) const
   {
   return numerator == o.numerator && denominator == o.denominator;
   }
bool Rat::operator!=(const Rat& o) const
   {
   return !(*this == o);
   }
bool Rat::operator<(const Rat& o) const
   {
   return numerator * o.denominator < o.numerator * denominator;
   }
bool Rat::operator<=(const Rat& o) const
   {
   return numerator * o.denominator <= o.numerator * denominator;
   }
bool Rat::operator>(const Rat& o) const
   {
   return numerator * o.denominator > o.numerator * denominator;
   }
bool Rat::operator>=(const Rat& o) const
   {
   return numerator * o.denominator >= o.numerator * denominator;
   }

// ── Comparisons: Rat vs int64_t ───────────────────────────────────────────────

bool Rat::operator==(int64_t n) const
   {
   return denominator == 1 && numerator == n;
   }
bool Rat::operator!=(int64_t n) const
   {
   return !(*this == n);
   }
bool Rat::operator<(int64_t n) const
   {
   return numerator < n * denominator;
   }
bool Rat::operator<=(int64_t n) const
   {
   return numerator <= n * denominator;
   }
bool Rat::operator>(int64_t n) const
   {
   return numerator > n * denominator;
   }
bool Rat::operator>=(int64_t n) const
   {
   return numerator >= n * denominator;
   }

// ── Comparisons: Rat vs double ────────────────────────────────────────────────

bool Rat::operator==(double f) const
   {
   return static_cast<double>(*this) == f;
   }
bool Rat::operator!=(double f) const
   {
   return static_cast<double>(*this) != f;
   }
bool Rat::operator<(double f) const
   {
   return static_cast<double>(*this) < f;
   }
bool Rat::operator<=(double f) const
   {
   return static_cast<double>(*this) <= f;
   }
bool Rat::operator>(double f) const
   {
   return static_cast<double>(*this) > f;
   }
bool Rat::operator>=(double f) const
   {
   return static_cast<double>(*this) >= f;
   }

// ── repr ──────────────────────────────────────────────────────────────────────

std::string Rat::repr() const
   {
   return "_Rat(" + std::to_string(numerator) + ", " + std::to_string(denominator) + ')';
   }

// ── Reverse binary operations ─────────────────────────────────────────────────

Rat operator+(int64_t n, const Rat& r)
   {
   return Rat(n * r.denominator + r.numerator, r.denominator);
   }
Rat operator-(int64_t n, const Rat& r)
   {
   return Rat(n * r.denominator - r.numerator, r.denominator);
   }
Rat operator*(int64_t n, const Rat& r)
   {
   return Rat(n * r.numerator, r.denominator);
   }
Rat operator/(int64_t n, const Rat& r)
   {
   return Rat(n * r.denominator, r.numerator);
   }
double operator+(double f, const Rat& r)
   {
   return f + static_cast<double>(r);
   }
double operator-(double f, const Rat& r)
   {
   return f - static_cast<double>(r);
   }
double operator*(double f, const Rat& r)
   {
   return f * static_cast<double>(r);
   }
double operator/(double f, const Rat& r)
   {
   return f / static_cast<double>(r);
   }

// ── Reverse comparisons ───────────────────────────────────────────────────────

bool operator==(int64_t n, const Rat& r)
   {
   return r == n;
   }
bool operator!=(int64_t n, const Rat& r)
   {
   return r != n;
   }
bool operator<(int64_t n, const Rat& r)
   {
   return r > n;
   }
bool operator<=(int64_t n, const Rat& r)
   {
   return r >= n;
   }
bool operator>(int64_t n, const Rat& r)
   {
   return r < n;
   }
bool operator>=(int64_t n, const Rat& r)
   {
   return r <= n;
   }
bool operator==(double f, const Rat& r)
   {
   return r == f;
   }
bool operator!=(double f, const Rat& r)
   {
   return r != f;
   }
bool operator<(double f, const Rat& r)
   {
   return r > f;
   }
bool operator<=(double f, const Rat& r)
   {
   return r >= f;
   }
bool operator>(double f, const Rat& r)
   {
   return r < f;
   }
bool operator>=(double f, const Rat& r)
   {
   return r <= f;
   }

// ── rat_abs ───────────────────────────────────────────────────────────────────

Rat rat_abs(const Rat& r)
   {
   if (r.numerator >= 0)
      return r;
   return Rat(-r.numerator, r.denominator);
   }

// ── Floor / ceil / trunc / round ─────────────────────────────────────────────
// Denominator is always positive after normalization.

int64_t rat_floor(const Rat& r)
   {
   // Port of __floor__: Python n // d (floor toward -inf)
   int64_t q = r.numerator / r.denominator;
   if (r.numerator % r.denominator != 0 && r.numerator < 0)
      q -= 1;
   return q;
   }

int64_t rat_ceil(const Rat& r)
   {
   // Port of __ceil__: Python -(-n // d)
   int64_t q = r.numerator / r.denominator;
   if (r.numerator % r.denominator != 0 && r.numerator > 0)
      q += 1;
   return q;
   }

int64_t rat_trunc(const Rat& r)
   {
   // Port of __trunc__: truncate toward zero -- same as C++ integer division
   return r.numerator / r.denominator;
   }

int64_t rat_round(const Rat& r)
   {
   // Port of __round__ (ndigits=None): round half to even (banker's rounding)
   int64_t floor_n = rat_floor(r);
   int64_t rem = r.numerator - floor_n * r.denominator;
   int64_t twice = 2 * rem;
   if (twice < r.denominator)
      return floor_n;
   if (twice > r.denominator)
      return floor_n + 1;
   return (floor_n % 2 == 0) ? floor_n : floor_n + 1;
   }
