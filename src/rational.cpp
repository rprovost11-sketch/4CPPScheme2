// rational.cpp -- Exact rational arithmetic type (arbitrary precision via mpz).
// Port of pyscheme/rational.py _Rat.  Phase 2 of the bignum work: numerator
// and denominator are mini-gmp mpz integers, so rational arithmetic never
// overflows (matching pyScheme's arbitrary-precision _Rat).
#include "rational.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <climits>
#include <cinttypes>

// ── int64 <-> mpz helpers (MSVC `long` is 32-bit, so route int64 via string) ──

static void _set_i64(__mpz_struct* z, int64_t n)
   {
   uint64_t mag = (n < 0) ? (~static_cast<uint64_t>(n) + 1u) : static_cast<uint64_t>(n);
   char buf[24];
   std::snprintf(buf, sizeof(buf), "%s%" PRIu64, (n < 0 ? "-" : ""), mag);
   mpz_set_str(z, buf, 10);
   }

// Truncating int64 of an mpz; clamps when out of range (legacy accessors only).
static int64_t _get_i64(const __mpz_struct* z)
   {
   if (mpz_sizeinbase(z, 2) <= 63)
      {
      char buf[24];
      mpz_get_str(buf, 10, z);
      return std::strtoll(buf, nullptr, 10);
      }
   return mpz_sgn(z) < 0 ? INT64_MIN : INT64_MAX;
   }

// Reduce to lowest terms with den > 0.  gcd(0,d)=d so 0/d -> 0/1.
static void _normalize(__mpz_struct* num, __mpz_struct* den)
   {
   if (mpz_sgn(den) < 0)
      {
      mpz_neg(num, num);
      mpz_neg(den, den);
      }
   __mpz_struct g;
   mpz_init(&g);
   mpz_gcd(&g, num, den);
   if (mpz_sgn(&g) != 0 && mpz_cmp_si(&g, 1) != 0)
      {
      mpz_divexact(num, num, &g);
      mpz_divexact(den, den, &g);
      }
   mpz_clear(&g);
   }

// ── Constructors / RAII ───────────────────────────────────────────────────────

Rat::Rat()
   {
   mpz_init_set_si(&num_, 0);
   mpz_init_set_si(&den_, 1);
   }

Rat::Rat(int64_t num, int64_t den)
   {
   mpz_init(&num_);
   mpz_init(&den_);
   if (den == 0)
      {
      mpz_clear(&num_);
      mpz_clear(&den_);
      throw std::domain_error("rational denominator is zero");
      }
   _set_i64(&num_, num);
   _set_i64(&den_, den);
   _normalize(&num_, &den_);
   }

Rat::Rat(const __mpz_struct* num, const __mpz_struct* den)
   {
   if (mpz_sgn(den) == 0)
      throw std::domain_error("rational denominator is zero");
   mpz_init_set(&num_, num);
   mpz_init_set(&den_, den);
   _normalize(&num_, &den_);
   }

Rat::Rat(const Rat& o)
   {
   mpz_init_set(&num_, &o.num_);
   mpz_init_set(&den_, &o.den_);
   }

Rat::Rat(Rat&& o) noexcept
   {
   mpz_init(&num_);
   mpz_init(&den_);
   mpz_swap(&num_, &o.num_);
   mpz_swap(&den_, &o.den_);
   }

Rat& Rat::operator=(const Rat& o)
   {
   if (this != &o)
      {
      mpz_set(&num_, &o.num_);
      mpz_set(&den_, &o.den_);
      }
   return *this;
   }

Rat& Rat::operator=(Rat&& o) noexcept
   {
   mpz_swap(&num_, &o.num_);
   mpz_swap(&den_, &o.den_);
   return *this;
   }

Rat::~Rat()
   {
   mpz_clear(&num_);
   mpz_clear(&den_);
   }

// Port of _Rat.from_float: a finite double is exactly sig * 2^e (sig a signed
// 53-bit integer).  Built exactly, so large doubles yield exact bignum rationals.
Rat Rat::from_float(double f)
   {
   if (f == 0.0)
      return Rat(0, 1);
   int bexp;
   double mant = std::frexp(f, &bexp); // f = mant * 2^bexp, |mant| in [0.5,1)
   double sigd = std::ldexp(std::fabs(mant), 53); // exact 53-bit integer
   int e = bexp - 53;                  // f = sig * 2^e
   __mpz_struct n, d;
   mpz_init(&n);
   mpz_init_set_si(&d, 1);
   mpz_set_d(&n, sigd); // exact (sigd is integral)
   if (f < 0.0)
      mpz_neg(&n, &n);
   if (e >= 0)
      mpz_mul_2exp(&n, &n, static_cast<mp_bitcnt_t>(e));
   else
      mpz_mul_2exp(&d, &d, static_cast<mp_bitcnt_t>(-e));
   Rat r(&n, &d);
   mpz_clear(&n);
   mpz_clear(&d);
   return r;
   }

// ── Predicates / conversions ──────────────────────────────────────────────────

bool Rat::is_zero() const { return mpz_sgn(&num_) == 0; }
bool Rat::is_integer() const { return mpz_cmp_si(&den_, 1) == 0; }

Rat::operator double() const
   {
   return mpz_get_d(&num_) / mpz_get_d(&den_);
   }
Rat::operator int64_t() const
   {
   __mpz_struct q;
   mpz_init(&q);
   mpz_tdiv_q(&q, &num_, &den_); // truncate toward zero
   int64_t v = _get_i64(&q);
   mpz_clear(&q);
   return v;
   }
Rat::operator bool() const { return mpz_sgn(&num_) != 0; }

int64_t Rat::numerator() const { return _get_i64(&num_); }
int64_t Rat::denominator() const { return _get_i64(&den_); }

// ── Unary ─────────────────────────────────────────────────────────────────────

Rat Rat::operator-() const
   {
   __mpz_struct n;
   mpz_init(&n);
   mpz_neg(&n, &num_);
   Rat r(&n, &den_); // den_ already positive & reduced
   mpz_clear(&n);
   return r;
   }
Rat Rat::operator+() const { return *this; }

// ── Binary: Rat op Rat ────────────────────────────────────────────────────────

Rat Rat::operator+(const Rat& o) const
   {
   __mpz_struct n, d, t;
   mpz_init(&n);
   mpz_init(&d);
   mpz_init(&t);
   mpz_mul(&n, &num_, &o.den_);
   mpz_mul(&t, &o.num_, &den_);
   mpz_add(&n, &n, &t);
   mpz_mul(&d, &den_, &o.den_);
   Rat r(&n, &d);
   mpz_clear(&n);
   mpz_clear(&d);
   mpz_clear(&t);
   return r;
   }
Rat Rat::operator-(const Rat& o) const
   {
   __mpz_struct n, d, t;
   mpz_init(&n);
   mpz_init(&d);
   mpz_init(&t);
   mpz_mul(&n, &num_, &o.den_);
   mpz_mul(&t, &o.num_, &den_);
   mpz_sub(&n, &n, &t);
   mpz_mul(&d, &den_, &o.den_);
   Rat r(&n, &d);
   mpz_clear(&n);
   mpz_clear(&d);
   mpz_clear(&t);
   return r;
   }
Rat Rat::operator*(const Rat& o) const
   {
   __mpz_struct n, d;
   mpz_init(&n);
   mpz_init(&d);
   mpz_mul(&n, &num_, &o.num_);
   mpz_mul(&d, &den_, &o.den_);
   Rat r(&n, &d);
   mpz_clear(&n);
   mpz_clear(&d);
   return r;
   }
Rat Rat::operator/(const Rat& o) const
   {
   // (num/den) / (o.num/o.den) = (num*o.den) / (den*o.num); ctor normalizes
   // sign and throws if o is zero (den becomes zero).
   __mpz_struct n, d;
   mpz_init(&n);
   mpz_init(&d);
   mpz_mul(&n, &num_, &o.den_);
   mpz_mul(&d, &den_, &o.num_);
   Rat r(&n, &d);
   mpz_clear(&n);
   mpz_clear(&d);
   return r;
   }

// ── Binary: Rat op int64_t (delegate through a temporary Rat) ──────────────────

Rat Rat::operator+(int64_t n) const { return *this + Rat(n); }
Rat Rat::operator-(int64_t n) const { return *this - Rat(n); }
Rat Rat::operator*(int64_t n) const { return *this * Rat(n); }
Rat Rat::operator/(int64_t n) const { return *this / Rat(n); }

// ── Binary: Rat op double ─────────────────────────────────────────────────────

double Rat::operator+(double f) const { return static_cast<double>(*this) + f; }
double Rat::operator-(double f) const { return static_cast<double>(*this) - f; }
double Rat::operator*(double f) const { return static_cast<double>(*this) * f; }
double Rat::operator/(double f) const { return static_cast<double>(*this) / f; }

// ── Power ─────────────────────────────────────────────────────────────────────

Rat Rat::pow(int exp) const
   {
   if (exp == 0)
      return Rat(1);
   __mpz_struct n, d;
   mpz_init(&n);
   mpz_init(&d);
   if (exp > 0)
      {
      mpz_pow_ui(&n, &num_, static_cast<unsigned long>(exp));
      mpz_pow_ui(&d, &den_, static_cast<unsigned long>(exp));
      }
   else
      {
      unsigned long p = static_cast<unsigned long>(-exp);
      mpz_pow_ui(&d, &num_, p); // invert
      mpz_pow_ui(&n, &den_, p);
      }
   Rat r(&n, &d);
   mpz_clear(&n);
   mpz_clear(&d);
   return r;
   }

// ── Comparisons: Rat vs Rat ───────────────────────────────────────────────────

// Sign of (a - b); denominators are positive so cross-multiplication preserves sign.
static int _rat_cmp(const Rat& a, const Rat& b)
   {
   __mpz_struct l, r;
   mpz_init(&l);
   mpz_init(&r);
   mpz_mul(&l, a.num(), b.den());
   mpz_mul(&r, b.num(), a.den());
   int c = mpz_cmp(&l, &r);
   mpz_clear(&l);
   mpz_clear(&r);
   return c;
   }

bool Rat::operator==(const Rat& o) const
   {
   return mpz_cmp(&num_, &o.num_) == 0 && mpz_cmp(&den_, &o.den_) == 0;
   }
bool Rat::operator!=(const Rat& o) const { return !(*this == o); }
bool Rat::operator<(const Rat& o) const { return _rat_cmp(*this, o) < 0; }
bool Rat::operator<=(const Rat& o) const { return _rat_cmp(*this, o) <= 0; }
bool Rat::operator>(const Rat& o) const { return _rat_cmp(*this, o) > 0; }
bool Rat::operator>=(const Rat& o) const { return _rat_cmp(*this, o) >= 0; }

// ── Comparisons: Rat vs int64_t (delegate) ────────────────────────────────────

bool Rat::operator==(int64_t n) const { return *this == Rat(n); }
bool Rat::operator!=(int64_t n) const { return !(*this == Rat(n)); }
bool Rat::operator<(int64_t n) const { return *this < Rat(n); }
bool Rat::operator<=(int64_t n) const { return *this <= Rat(n); }
bool Rat::operator>(int64_t n) const { return *this > Rat(n); }
bool Rat::operator>=(int64_t n) const { return *this >= Rat(n); }

// ── Comparisons: Rat vs double ────────────────────────────────────────────────

bool Rat::operator==(double f) const { return static_cast<double>(*this) == f; }
bool Rat::operator!=(double f) const { return static_cast<double>(*this) != f; }
bool Rat::operator<(double f) const { return static_cast<double>(*this) < f; }
bool Rat::operator<=(double f) const { return static_cast<double>(*this) <= f; }
bool Rat::operator>(double f) const { return static_cast<double>(*this) > f; }
bool Rat::operator>=(double f) const { return static_cast<double>(*this) >= f; }

// ── repr ──────────────────────────────────────────────────────────────────────

std::string Rat::repr() const
   {
   std::string ns(mpz_sizeinbase(&num_, 10) + 2, '\0');
   std::string ds(mpz_sizeinbase(&den_, 10) + 2, '\0');
   mpz_get_str(&ns[0], 10, &num_);
   mpz_get_str(&ds[0], 10, &den_);
   ns.resize(std::strlen(ns.c_str()));
   ds.resize(std::strlen(ds.c_str()));
   return "_Rat(" + ns + ", " + ds + ')';
   }

// ── Reverse binary operations ─────────────────────────────────────────────────

Rat operator+(int64_t n, const Rat& r) { return Rat(n) + r; }
Rat operator-(int64_t n, const Rat& r) { return Rat(n) - r; }
Rat operator*(int64_t n, const Rat& r) { return Rat(n) * r; }
Rat operator/(int64_t n, const Rat& r) { return Rat(n) / r; }
double operator+(double f, const Rat& r) { return f + static_cast<double>(r); }
double operator-(double f, const Rat& r) { return f - static_cast<double>(r); }
double operator*(double f, const Rat& r) { return f * static_cast<double>(r); }
double operator/(double f, const Rat& r) { return f / static_cast<double>(r); }

// ── Reverse comparisons ───────────────────────────────────────────────────────

bool operator==(int64_t n, const Rat& r) { return r == n; }
bool operator!=(int64_t n, const Rat& r) { return r != n; }
bool operator<(int64_t n, const Rat& r) { return r > n; }
bool operator<=(int64_t n, const Rat& r) { return r >= n; }
bool operator>(int64_t n, const Rat& r) { return r < n; }
bool operator>=(int64_t n, const Rat& r) { return r <= n; }
bool operator==(double f, const Rat& r) { return r == f; }
bool operator!=(double f, const Rat& r) { return r != f; }
bool operator<(double f, const Rat& r) { return r > f; }
bool operator<=(double f, const Rat& r) { return r >= f; }
bool operator>(double f, const Rat& r) { return r < f; }
bool operator>=(double f, const Rat& r) { return r <= f; }

// ── rat_abs ───────────────────────────────────────────────────────────────────

Rat rat_abs(const Rat& r)
   {
   if (mpz_sgn(r.num()) >= 0)
      return r;
   __mpz_struct n;
   mpz_init(&n);
   mpz_neg(&n, r.num());
   Rat result(&n, r.den());
   mpz_clear(&n);
   return result;
   }

// ── Floor / ceil / trunc / round (int64 result; denominator > 0) ──────────────

int64_t rat_floor(const Rat& r)
   {
   __mpz_struct q;
   mpz_init(&q);
   mpz_fdiv_q(&q, r.num(), r.den()); // floor toward -inf
   int64_t v = _get_i64(&q);
   mpz_clear(&q);
   return v;
   }

int64_t rat_ceil(const Rat& r)
   {
   __mpz_struct q;
   mpz_init(&q);
   mpz_cdiv_q(&q, r.num(), r.den());
   int64_t v = _get_i64(&q);
   mpz_clear(&q);
   return v;
   }

int64_t rat_trunc(const Rat& r)
   {
   __mpz_struct q;
   mpz_init(&q);
   mpz_tdiv_q(&q, r.num(), r.den()); // toward zero
   int64_t v = _get_i64(&q);
   mpz_clear(&q);
   return v;
   }

int64_t rat_round(const Rat& r)
   {
   // round half to even (banker's rounding)
   __mpz_struct q, rem, twice;
   mpz_init(&q);
   mpz_init(&rem);
   mpz_init(&twice);
   mpz_fdiv_qr(&q, &rem, r.num(), r.den()); // q=floor, 0 <= rem < den
   mpz_mul_2exp(&twice, &rem, 1);           // 2*rem
   int c = mpz_cmp(&twice, r.den());
   int64_t fl = _get_i64(&q);
   int64_t result;
   if (c < 0)
      result = fl;
   else if (c > 0)
      result = fl + 1;
   else
      result = (mpz_even_p(&q) ? fl : fl + 1); // halfway -> round to even
   mpz_clear(&q);
   mpz_clear(&rem);
   mpz_clear(&twice);
   return result;
   }
