// rat_test.cpp -- white-box unit tests for the numeric tower.
//
// Tests are self-contained C++ that directly drive the arbitrary-precision
// numeric machinery -- the `Rat` exact-rational type (rational.h) and the
// Value-level exact constructors make_rational_mpz / make_bignum* (AST.h).
// Scheme is not involved: these are cppScheme2-only modules (pyScheme rides on
// Python's native int / fractions.Fraction), so there is no cross-port parity
// partner and their invariants -- gcd reduction, sign normalization (den > 0),
// the den==1 -> integer collapse, and the int64<->mpz string routing that
// dodges MSVC's 32-bit `long` (LLP64) -- are only exercised indirectly by the
// black-box suites.  This harness pins them directly.
//
// Modeled on undercarriage-tests/gc_test.cpp (same tiny TEST/CHECK framework).

#include "rational.h"
#include "AST.h"
#include "Interpreter.h"
#include "mini-gmp/mini-gmp.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// ── Tiny test framework (shared shape with gc_test.cpp) ───────────────────────

struct TestCase
   {
   const char* name;
   void (*fn)();
   };

static std::vector<TestCase>& tests()
   {
   static std::vector<TestCase> v;
   return v;
   }

static int g_failures = 0;
static const char* g_current_test = nullptr;

struct TestRegistrar
   {
   TestRegistrar(const char* name, void (*fn)()) { tests().push_back({name, fn}); }
   };

#define TEST(NAME)                                \
   static void NAME();                            \
   static TestRegistrar _reg_##NAME(#NAME, NAME); \
   static void NAME()

#define CHECK(COND)                                               \
   do                                                             \
      {                                                           \
      if (!(COND))                                                \
         {                                                        \
         std::fprintf(stderr, "  FAIL [%s] %s:%d: %s\n",          \
                      g_current_test, __FILE__, __LINE__, #COND); \
         ++g_failures;                                            \
         }                                                        \
      } while (0)

// String-valued equality with both sides printed (used for mpz readbacks).
#define CHECK_STR(ACTUAL, EXPECTED)                                          \
   do                                                                        \
      {                                                                      \
      std::string _a = (ACTUAL);                                            \
      std::string _e = (EXPECTED);                                          \
      if (_a != _e)                                                          \
         {                                                                   \
         std::fprintf(stderr, "  FAIL [%s] %s:%d: %s   (got \"%s\" want \"%s\")\n", \
                      g_current_test, __FILE__, __LINE__, #ACTUAL,           \
                      _a.c_str(), _e.c_str());                               \
         ++g_failures;                                                       \
         }                                                                   \
      } while (0)

// ── Helpers ───────────────────────────────────────────────────────────────────

// Decimal string of an mpz component.
static std::string S(const __mpz_struct* z) { return mpz_to_string(z, 10); }

// Decimal strings of a Rat's exact numerator / denominator.
static std::string num_s(const Rat& r) { return S(r.num()); }
static std::string den_s(const Rat& r) { return S(r.den()); }

// Build an mpz from a decimal string; caller must mpz_clear it.
static void mpz_from(const char* s, __mpz_struct* out)
   {
   mpz_init(out);
   mpz_set_str(out, s, 10);
   }

// ── Rat: construction & normalization ─────────────────────────────────────────

TEST(rat_default_is_zero_over_one)
   {
   Rat r;
   CHECK(r.is_zero());
   CHECK(r.is_integer());
   CHECK_STR(num_s(r), "0");
   CHECK_STR(den_s(r), "1");
   }

TEST(rat_reduces_to_lowest_terms)
   {
   CHECK_STR(num_s(Rat(6, 4)), "3"); // 6/4 -> 3/2
   CHECK_STR(den_s(Rat(6, 4)), "2");
   CHECK_STR(num_s(Rat(100, 35)), "20"); // gcd 5 -> 20/7
   CHECK_STR(den_s(Rat(100, 35)), "7");
   }

TEST(rat_normalizes_sign_to_numerator)
   {
   // Denominator must always end up > 0; the sign rides the numerator.
   CHECK_STR(num_s(Rat(2, -4)), "-1");
   CHECK_STR(den_s(Rat(2, -4)), "2");
   CHECK_STR(num_s(Rat(-2, -4)), "1"); // double negative
   CHECK_STR(den_s(Rat(-2, -4)), "2");
   CHECK_STR(num_s(Rat(-2, 4)), "-1");
   CHECK_STR(den_s(Rat(-2, 4)), "2");
   }

TEST(rat_zero_numerator_reduces_denominator_to_one)
   {
   // gcd(0, d) == d, so 0/d must collapse to the canonical 0/1.
   Rat r(0, 5);
   CHECK(r.is_zero());
   CHECK(r.is_integer());
   CHECK_STR(den_s(r), "1");
   }

TEST(rat_integer_valued_has_denominator_one)
   {
   Rat r(5, 1);
   CHECK(r.is_integer());
   CHECK(!r.is_zero());
   CHECK_STR(num_s(r), "5");
   CHECK_STR(den_s(r), "1");
   CHECK(r == (int64_t)5);
   }

TEST(rat_zero_denominator_throws)
   {
   bool threw = false;
   try
      {
      Rat bad(1, 0);
      (void)bad;
      }
   catch (const std::domain_error&)
      {
      threw = true;
      }
   CHECK(threw);
   }

// ── Rat: arithmetic ───────────────────────────────────────────────────────────

TEST(rat_addition)
   {
   Rat r = Rat(1, 2) + Rat(1, 3); // 5/6
   CHECK_STR(num_s(r), "5");
   CHECK_STR(den_s(r), "6");
   }

TEST(rat_subtraction_to_zero)
   {
   Rat r = Rat(1, 2) - Rat(1, 2);
   CHECK(r.is_zero());
   CHECK_STR(den_s(r), "1");
   }

TEST(rat_multiplication_reduces)
   {
   Rat r = Rat(2, 3) * Rat(3, 4); // 6/12 -> 1/2
   CHECK_STR(num_s(r), "1");
   CHECK_STR(den_s(r), "2");
   }

TEST(rat_division_to_integer)
   {
   Rat r = Rat(1, 2) / Rat(1, 4); // -> 2
   CHECK(r.is_integer());
   CHECK(r == (int64_t)2);
   }

TEST(rat_negate_and_abs)
   {
   CHECK_STR(num_s(-Rat(3, 4)), "-3");
   CHECK_STR(num_s(rat_abs(Rat(-3, 4))), "3");
   CHECK_STR(den_s(rat_abs(Rat(-3, 4))), "4");
   }

TEST(rat_power)
   {
   Rat r = Rat(2, 3).pow(3); // 8/27
   CHECK_STR(num_s(r), "8");
   CHECK_STR(den_s(r), "27");
   CHECK(Rat(2, 3).pow(0) == (int64_t)1); // x^0 = 1
   Rat inv = Rat(2, 3).pow(-1);           // reciprocal
   CHECK_STR(num_s(inv), "3");
   CHECK_STR(den_s(inv), "2");
   }

// ── Rat: comparisons & mixed-type ops ─────────────────────────────────────────

TEST(rat_equality_independent_of_unreduced_form)
   {
   CHECK(Rat(1, 2) == Rat(2, 4));
   CHECK(Rat(1, 2) != Rat(2, 3));
   CHECK(Rat(1, 2) < Rat(2, 3));
   CHECK(Rat(2, 3) > Rat(1, 2));
   }

TEST(rat_compares_against_int_and_double)
   {
   CHECK(Rat(3, 2) == 1.5);
   CHECK(Rat(7, 2) > (int64_t)3);
   CHECK(Rat(6, 2) == (int64_t)3);
   CHECK((double)Rat(1, 4) == 0.25);
   CHECK((int64_t)Rat(7, 2) == 3); // truncates toward zero
   CHECK((int64_t)Rat(-7, 2) == -3);
   }

// ── Rat::from_float ───────────────────────────────────────────────────────────

TEST(rat_from_float_exact_dyadic)
   {
   // Powers-of-two denominators are exactly representable.
   CHECK(Rat::from_float(0.5) == Rat(1, 2));
   CHECK(Rat::from_float(0.25) == Rat(1, 4));
   CHECK(Rat::from_float(-0.75) == Rat(-3, 4));
   Rat two = Rat::from_float(2.0);
   CHECK(two.is_integer());
   CHECK(two == (int64_t)2);
   }

// ── int64 <-> mpz routing (must not truncate on MSVC's 32-bit long) ───────────

TEST(rat_int64_max_not_truncated)
   {
   // If the int64->mpz conversion went through `long` (32-bit on Win64) it would
   // truncate; the string route must preserve the full 64-bit magnitude.
   Rat r(9223372036854775807LL, 1); // INT64_MAX
   CHECK(r.is_integer());
   CHECK_STR(num_s(r), "9223372036854775807");
   }

TEST(rat_large_int64_numerator_and_denominator)
   {
   Rat r(9223372036854775806LL, 2); // (2^63-2)/2 -> (2^62-1)/1
   CHECK(r.is_integer());
   CHECK_STR(num_s(r), "4611686018427387903");
   CHECK_STR(den_s(r), "1");
   }

// ── Bignum Rat: components beyond int64 (the Phase-2 tower) ────────────────────

TEST(rat_bignum_components_roundtrip)
   {
   // num = 2^100, den = 3 (coprime) -> stays 2^100 / 3 exactly.
   __mpz_struct n, d;
   mpz_from("1267650600228229401496703205376", &n); // 2^100
   mpz_from("3", &d);
   Rat r(&n, &d);
   CHECK_STR(num_s(r), "1267650600228229401496703205376");
   CHECK_STR(den_s(r), "3");
   CHECK(!r.is_integer());
   mpz_clear(&n);
   mpz_clear(&d);
   }

TEST(rat_bignum_reduces_via_gcd)
   {
   // (2 * 2^100) / (2 * 3) must reduce by gcd 2 back to 2^100 / 3.
   __mpz_struct n, d;
   mpz_from("2535301200456458802993406410752", &n); // 2^101
   mpz_from("6", &d);
   Rat r(&n, &d);
   CHECK_STR(num_s(r), "1267650600228229401496703205376"); // 2^100
   CHECK_STR(den_s(r), "3");
   mpz_clear(&n);
   mpz_clear(&d);
   }

// ── Value level: make_rational_mpz collapse & sign normalization ──────────────

// Build a Value rational from two decimal strings via the mpz constructor.
static Value rat_value(const char* n_s, const char* d_s)
   {
   __mpz_struct n, d;
   mpz_from(n_s, &n);
   mpz_from(d_s, &d);
   Value v = make_rational_mpz(&n, &d);
   mpz_clear(&n);
   mpz_clear(&d);
   return v;
   }

TEST(make_rational_mpz_reduces)
   {
   Value v = rat_value("6", "4"); // -> 3/2
   CHECK(is_rational(v));
   CHECK_STR(S(as_rational_num(v)), "3");
   CHECK_STR(S(as_rational_den(v)), "2");
   }

TEST(make_rational_mpz_collapses_to_integer_when_den_one)
   {
   Value v = rat_value("10", "5"); // -> 2, an exact integer (not a rational)
   CHECK(!is_rational(v));
   CHECK(is_integer(v));
   CHECK(as_integer(v) == 2);
   }

TEST(make_rational_mpz_zero_collapses_to_integer_zero)
   {
   Value v = rat_value("0", "5");
   CHECK(!is_rational(v));
   CHECK(is_integer(v));
   CHECK(as_integer(v) == 0);
   }

TEST(make_rational_mpz_normalizes_negative_denominator)
   {
   Value v = rat_value("1", "-2"); // -> -1/2 with den > 0
   CHECK(is_rational(v));
   CHECK_STR(S(as_rational_num(v)), "-1");
   CHECK_STR(S(as_rational_den(v)), "2");
   }

TEST(make_rational_mpz_bignum_integer_collapse)
   {
   // 2^100 / 1 -> an exact integer that exceeds int64 -> a bignum, not a rational.
   Value v = rat_value("1267650600228229401496703205376", "1");
   CHECK(!is_rational(v));
   CHECK(is_bignum(v));
   CHECK_STR(bignum_to_string(v), "1267650600228229401496703205376");
   }

TEST(make_rational_mpz_keeps_bignum_rational)
   {
   Value v = rat_value("1", "1267650600228229401496703205376"); // 1 / 2^100
   CHECK(is_rational(v));
   CHECK_STR(S(as_rational_num(v)), "1");
   CHECK_STR(S(as_rational_den(v)), "1267650600228229401496703205376");
   }

// ── Value level: bignum constructors ──────────────────────────────────────────

TEST(make_bignum_str_roundtrips_decimal_and_hex)
   {
   Value v = make_bignum_str("1267650600228229401496703205376", 10); // 2^100
   CHECK(is_bignum(v));
   CHECK_STR(bignum_to_string(v, 10), "1267650600228229401496703205376");
   CHECK_STR(bignum_to_string(v, 16), "10000000000000000000000000"); // 2^100 in hex
   }

TEST(make_bignum_si_preserves_full_int64)
   {
   Value v = make_bignum_si(9223372036854775807LL); // INT64_MAX
   CHECK(is_bignum(v));
   CHECK_STR(bignum_to_string(v), "9223372036854775807");
   }

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv)
   {
   const char* filter = nullptr;
   for (int i = 1; i < argc; ++i)
      if (std::strncmp(argv[i], "--filter=", 9) == 0)
         filter = argv[i] + 9;

   // One interpreter ensures every subsystem the value constructors touch
   // (symbol pool, GC, object pools) is fully initialized, as in normal runs.
   Interpreter interp;

   std::printf("Running %zu numeric-tower tests%s%s...\n", tests().size(),
               filter ? " filtered by " : "", filter ? filter : "");
   for (const auto& tc : tests())
      {
      if (filter && !std::strstr(tc.name, filter))
         continue;
      g_current_test = tc.name;
      int before = g_failures;
      tc.fn();
      std::printf("  [%s] %s\n", (g_failures == before) ? "ok" : "FAIL", tc.name);
      }
   std::printf("\n%d failure(s)\n", g_failures);
   return g_failures == 0 ? 0 : 1;
   }
