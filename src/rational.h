#pragma once
// rational.h -- Exact rational arithmetic type.
// Direct port of pyscheme/rational.py _Rat.
// Stack-allocated value type used by the evaluator for rational arithmetic.
// Results are converted back to Value via make_rational() in AST.h.
#include "scheme_export.h"
#include <cstdint>
#include <stdexcept>
#include <string>

// ── Rat ───────────────────────────────────────────────────────────────────────
// Port of rational.py _Rat.
// Invariants after construction: denominator > 0, gcd(|numerator|,denominator) == 1.
// Uses int64_t for both fields; may lose precision for extreme double values in
// from_float (unlike Python's arbitrary-precision int).

struct CPPSCHEME2_API Rat {
    int64_t numerator;
    int64_t denominator;

    Rat();                              // 0/1
    Rat(int64_t num, int64_t den = 1); // normalizes; throws std::domain_error if den==0

    // Port of _Rat.from_float: converts double to nearest exact rational.
    static Rat from_float(double f);

    // Conversions (port of __float__, __int__, __bool__)
    explicit operator double()   const;
    explicit operator int64_t()  const;  // truncate toward zero
    explicit operator bool()     const;

    // Unary (port of __neg__, __abs__, __pos__)
    Rat operator-() const;
    Rat operator+() const;

    // Binary: Rat op Rat (port of __add__, __sub__, __mul__, __truediv__)
    Rat operator+(const Rat& o) const;
    Rat operator-(const Rat& o) const;
    Rat operator*(const Rat& o) const;
    Rat operator/(const Rat& o) const;

    // Binary: Rat op int64_t (port of __add__(int), etc.)
    Rat    operator+(int64_t n) const;
    Rat    operator-(int64_t n) const;
    Rat    operator*(int64_t n) const;
    Rat    operator/(int64_t n) const;

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
    bool operator< (const Rat& o) const;
    bool operator<=(const Rat& o) const;
    bool operator> (const Rat& o) const;
    bool operator>=(const Rat& o) const;

    // Comparisons: Rat vs int64_t
    bool operator==(int64_t n) const;
    bool operator!=(int64_t n) const;
    bool operator< (int64_t n) const;
    bool operator<=(int64_t n) const;
    bool operator> (int64_t n) const;
    bool operator>=(int64_t n) const;

    // Comparisons: Rat vs double
    bool operator==(double f) const;
    bool operator!=(double f) const;
    bool operator< (double f) const;
    bool operator<=(double f) const;
    bool operator> (double f) const;
    bool operator>=(double f) const;

    // Debug display (port of __repr__)
    std::string repr() const;
};

// Reverse binary operations (port of __radd__, __rsub__, __rmul__, __rtruediv__)
CPPSCHEME2_API Rat    operator+(int64_t n, const Rat& r);
CPPSCHEME2_API Rat    operator-(int64_t n, const Rat& r);
CPPSCHEME2_API Rat    operator*(int64_t n, const Rat& r);
CPPSCHEME2_API Rat    operator/(int64_t n, const Rat& r);
CPPSCHEME2_API double operator+(double f,  const Rat& r);
CPPSCHEME2_API double operator-(double f,  const Rat& r);
CPPSCHEME2_API double operator*(double f,  const Rat& r);
CPPSCHEME2_API double operator/(double f,  const Rat& r);

// Reverse comparisons: int64_t vs Rat, double vs Rat
CPPSCHEME2_API bool operator==(int64_t n, const Rat& r);
CPPSCHEME2_API bool operator!=(int64_t n, const Rat& r);
CPPSCHEME2_API bool operator< (int64_t n, const Rat& r);
CPPSCHEME2_API bool operator<=(int64_t n, const Rat& r);
CPPSCHEME2_API bool operator> (int64_t n, const Rat& r);
CPPSCHEME2_API bool operator>=(int64_t n, const Rat& r);
CPPSCHEME2_API bool operator==(double f,  const Rat& r);
CPPSCHEME2_API bool operator!=(double f,  const Rat& r);
CPPSCHEME2_API bool operator< (double f,  const Rat& r);
CPPSCHEME2_API bool operator<=(double f,  const Rat& r);
CPPSCHEME2_API bool operator> (double f,  const Rat& r);
CPPSCHEME2_API bool operator>=(double f,  const Rat& r);

// rat_abs (port of __abs__)
CPPSCHEME2_API Rat rat_abs(const Rat& r);

// Floor/ceil/trunc/round (port of __floor__, __ceil__, __trunc__, __round__)
CPPSCHEME2_API int64_t rat_floor(const Rat& r);
CPPSCHEME2_API int64_t rat_ceil(const Rat& r);
CPPSCHEME2_API int64_t rat_trunc(const Rat& r);
CPPSCHEME2_API int64_t rat_round(const Rat& r);  // ndigits=None (return integer)
