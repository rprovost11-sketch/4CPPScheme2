#pragma once
// rational.h -- Exact rational arithmetic type.
// Port of pyscheme/rational.py _Rat.  Stack-allocated value type used by the
// evaluator for rational arithmetic.  Results are converted back to Value via
// make_rational() / rat_to_value() callers.
//
// Arbitrary precision: numerator and denominator are mini-gmp mpz integers
// (Phase 2 of the bignum work), matching pyScheme's arbitrary-precision _Rat.
// This makes Rat a non-trivial RAII type (each value owns two mpz); copies
// deep-copy.  The int64 accessors numerator()/denominator() truncate and are
// retained only for callers that still need int64 (e.g. the parser's rational
// literal token fields); use num()/den() for exact access.
#include "scheme_export.h"
#include "mini-gmp/mini-gmp.h"
#include <cstdint>
#include <stdexcept>
#include <string>

// ── Rat ───────────────────────────────────────────────────────────────────────
// Invariants after construction: denominator > 0, gcd(|numerator|,denominator) == 1.

struct CPPSCHEME2_API Rat
   {
   __mpz_struct num_; // numerator (carries the sign)
   __mpz_struct den_; // denominator, always > 0

   Rat();                             // 0/1
   Rat(int64_t num, int64_t den = 1); // normalizes; throws std::domain_error if den==0
   Rat(const __mpz_struct* num, const __mpz_struct* den); // from mpz pair; normalizes

   // RAII (each Rat owns two mpz).
   Rat(const Rat& o);
   Rat(Rat&& o) noexcept;
   Rat& operator=(const Rat& o);
   Rat& operator=(Rat&& o) noexcept;
   ~Rat();

   // Port of _Rat.from_float: converts double to its exact rational value.
   static Rat from_float(double f);

   // Exact component access (denominator always > 0).
   const __mpz_struct* num() const { return &num_; }
   const __mpz_struct* den() const { return &den_; }
   bool is_zero() const;      // numerator == 0
   bool is_integer() const;   // denominator == 1

   // Conversions (port of __float__, __int__, __bool__)
   explicit operator double() const;
   explicit operator int64_t() const; // truncate toward zero (may overflow)
   explicit operator bool() const;

   // Truncating int64 component access (legacy; lossy for bignum components).
   int64_t numerator() const;
   int64_t denominator() const;

   // Unary (port of __neg__, __abs__, __pos__)
   Rat operator-() const;
   Rat operator+() const;

   // Binary: Rat op Rat (port of __add__, __sub__, __mul__, __truediv__)
   Rat operator+(const Rat& o) const;
   Rat operator-(const Rat& o) const;
   Rat operator*(const Rat& o) const;
   Rat operator/(const Rat& o) const;

   // Binary: Rat op int64_t (port of __add__(int), etc.)
   Rat operator+(int64_t n) const;
   Rat operator-(int64_t n) const;
   Rat operator*(int64_t n) const;
   Rat operator/(int64_t n) const;

   // Binary: Rat op double (port of __add__(float), etc.)
   double operator+(double f) const;
   double operator-(double f) const;
   double operator*(double f) const;
   double operator/(double f) const;

   // Power (port of __pow__; exponent must be int)
   Rat pow(int exp) const;

   // Comparisons: Rat vs Rat
   bool operator==(const Rat& o) const;
   bool operator!=(const Rat& o) const;
   bool operator<(const Rat& o) const;
   bool operator<=(const Rat& o) const;
   bool operator>(const Rat& o) const;
   bool operator>=(const Rat& o) const;

   // Comparisons: Rat vs int64_t
   bool operator==(int64_t n) const;
   bool operator!=(int64_t n) const;
   bool operator<(int64_t n) const;
   bool operator<=(int64_t n) const;
   bool operator>(int64_t n) const;
   bool operator>=(int64_t n) const;

   // Comparisons: Rat vs double
   bool operator==(double f) const;
   bool operator!=(double f) const;
   bool operator<(double f) const;
   bool operator<=(double f) const;
   bool operator>(double f) const;
   bool operator>=(double f) const;

   // Debug display (port of __repr__)
   std::string repr() const;
   };

// Reverse binary operations (port of __radd__, __rsub__, __rmul__, __rtruediv__)
CPPSCHEME2_API Rat operator+(int64_t n, const Rat& r);
CPPSCHEME2_API Rat operator-(int64_t n, const Rat& r);
CPPSCHEME2_API Rat operator*(int64_t n, const Rat& r);
CPPSCHEME2_API Rat operator/(int64_t n, const Rat& r);
CPPSCHEME2_API double operator+(double f, const Rat& r);
CPPSCHEME2_API double operator-(double f, const Rat& r);
CPPSCHEME2_API double operator*(double f, const Rat& r);
CPPSCHEME2_API double operator/(double f, const Rat& r);

// Reverse comparisons: int64_t vs Rat, double vs Rat
CPPSCHEME2_API bool operator==(int64_t n, const Rat& r);
CPPSCHEME2_API bool operator!=(int64_t n, const Rat& r);
CPPSCHEME2_API bool operator<(int64_t n, const Rat& r);
CPPSCHEME2_API bool operator<=(int64_t n, const Rat& r);
CPPSCHEME2_API bool operator>(int64_t n, const Rat& r);
CPPSCHEME2_API bool operator>=(int64_t n, const Rat& r);
CPPSCHEME2_API bool operator==(double f, const Rat& r);
CPPSCHEME2_API bool operator!=(double f, const Rat& r);
CPPSCHEME2_API bool operator<(double f, const Rat& r);
CPPSCHEME2_API bool operator<=(double f, const Rat& r);
CPPSCHEME2_API bool operator>(double f, const Rat& r);
CPPSCHEME2_API bool operator>=(double f, const Rat& r);

// rat_abs (port of __abs__)
CPPSCHEME2_API Rat rat_abs(const Rat& r);

// Floor/ceil/trunc/round (port of __floor__, __ceil__, __trunc__, __round__).
// Return int64_t (truncating); used by floor/ceiling/truncate/round on
// non-integer rationals, whose values currently fit int64.
CPPSCHEME2_API int64_t rat_floor(const Rat& r);
CPPSCHEME2_API int64_t rat_ceil(const Rat& r);
CPPSCHEME2_API int64_t rat_trunc(const Rat& r);
CPPSCHEME2_API int64_t rat_round(const Rat& r); // ndigits=None (return integer)
