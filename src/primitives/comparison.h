#pragma once
// primitives/comparison.h -- numeric comparison primitives.
// Direct port of pyscheme/primitives/comparison.py.

struct Value;

// Sign of (a - b) for two real-number Values: -1, 0, 1, or -2 when unordered
// (a NaN is involved).  Bignum-aware and exact across the exact/inexact
// boundary (uses mpz_cmp_d).  Shared by the comparison primitives (=, <, >,
// <=, >=) and by min/max in arithmetic.cpp.
int scheme_real_cmp(const Value& a, const Value& b);
