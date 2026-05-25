// primitives/comparison.cpp -- numeric comparison primitives.
// Direct port of pyscheme/primitives/comparison.py.
#include "comparison.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"
#include "../rational.h"
#include <complex>
#include <variant>

static const char* CATEGORY = "comparison";

// ── NumReal: real-valued number extracted from a Value (for ordering) ─────────
// Port of comparison.py _num.

using NumReal = std::variant<int64_t, Rat, double>;

static double to_double(const NumReal& n) {
    return std::visit([](auto v) { return static_cast<double>(v); }, n);
}

static bool real_lt(const NumReal& a, const NumReal& b) {
    if (auto* ai = std::get_if<int64_t>(&a)) {
        if (auto* bi = std::get_if<int64_t>(&b)) return *ai < *bi;
        if (auto* br = std::get_if<Rat>(&b))     return *ai < *br;
    }
    if (auto* ar = std::get_if<Rat>(&a)) {
        if (auto* bi = std::get_if<int64_t>(&b)) return *ar < *bi;
        if (auto* br = std::get_if<Rat>(&b))     return *ar < *br;
    }
    return to_double(a) < to_double(b);
}

static bool real_le(const NumReal& a, const NumReal& b) {
    if (auto* ai = std::get_if<int64_t>(&a)) {
        if (auto* bi = std::get_if<int64_t>(&b)) return *ai <= *bi;
        if (auto* br = std::get_if<Rat>(&b))     return *ai <= *br;
    }
    if (auto* ar = std::get_if<Rat>(&a)) {
        if (auto* bi = std::get_if<int64_t>(&b)) return *ar <= *bi;
        if (auto* br = std::get_if<Rat>(&b))     return *ar <= *br;
    }
    return to_double(a) <= to_double(b);
}

static bool real_eq(const NumReal& a, const NumReal& b) {
    if (auto* ai = std::get_if<int64_t>(&a)) {
        if (auto* bi = std::get_if<int64_t>(&b)) return *ai == *bi;
        if (auto* br = std::get_if<Rat>(&b))     return *ai == *br;
    }
    if (auto* ar = std::get_if<Rat>(&a)) {
        if (auto* bi = std::get_if<int64_t>(&b)) return *ar == *bi;
        if (auto* br = std::get_if<Rat>(&b))     return *ar == *br;
    }
    return to_double(a) == to_double(b);
}

static NumReal _num(const Value& v, const char* name, const Value* app_node, int i) {
    if (is_integer(v))  return as_integer(v);
    if (is_bignum(v))   return mpz_get_d(as_bignum(v));
    if (is_rational(v)) return Rat{as_rational_num(v), as_rational_den(v)};
    if (is_real(v))     return as_real(v);
    if (is_complex(v) && as_complex_imag(v) == 0.0) return as_complex_real(v);
    if (is_exact_complex(v)) {
        Value im = as_exact_complex_imag(v);
        if (is_integer(im) && as_integer(im) == 0) {
            Value re = as_exact_complex_real(v);
            if (is_integer(re))  return as_integer(re);
            if (is_rational(re)) return Rat{as_rational_num(re), as_rational_den(re)};
        }
    }
    throw SchemeTypeError(
        std::string(name) + ": argument " + std::to_string(i) + " is not a real number",
        app_node ? src_of(*app_node) : nullptr);
}

// ── NumAny: any number (including complex) for = ──────────────────────────────
// Port of comparison.py _num_eq helper.

using NumAny = std::variant<int64_t, Rat, double, std::complex<double>>;

static NumAny _num_eq_val(const Value& v, const Value* app_node, int i) {
    if (is_integer(v))  return as_integer(v);
    if (is_bignum(v))   return mpz_get_d(as_bignum(v));
    if (is_rational(v)) return Rat{as_rational_num(v), as_rational_den(v)};
    if (is_real(v))     return as_real(v);
    if (is_complex(v))  return std::complex<double>(as_complex_real(v), as_complex_imag(v));
    if (is_exact_complex(v)) {
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

static bool any_eq(const NumAny& a, const NumAny& b) {
    // Convert both to complex<double> for equality check -- same as Python's cross-type ==.
    auto to_cx = [](const NumAny& n) -> std::complex<double> {
        return std::visit([](auto v) -> std::complex<double> {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::complex<double>>)
                return v;
            else
                return { static_cast<double>(v), 0.0 };
        }, n);
    };
    // Exact int/rat comparisons first.
    if (auto* ai = std::get_if<int64_t>(&a)) {
        if (auto* bi = std::get_if<int64_t>(&b)) return *ai == *bi;
        if (auto* br = std::get_if<Rat>(&b))     return *ai == *br;
    }
    if (auto* ar = std::get_if<Rat>(&a)) {
        if (auto* bi = std::get_if<int64_t>(&b)) return *ar == *bi;
        if (auto* br = std::get_if<Rat>(&b))     return *ar == *br;
    }
    return to_cx(a) == to_cx(b);
}

// ── Primitives ────────────────────────────────────────────────────────────────

static Value _prim_num_eq(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    NumAny prev = _num_eq_val(args[0], app, 1);
    for (int i = 1; i < static_cast<int>(args.size()); ++i) {
        NumAny cur = _num_eq_val(args[i], app, i + 1);
        if (!any_eq(prev, cur)) return make_boolean(false);
        prev = cur;
    }
    return make_boolean(true);
}

static Value _prim_num_lt(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    NumReal prev = _num(args[0], "<", app, 1);
    for (int i = 1; i < static_cast<int>(args.size()); ++i) {
        NumReal cur = _num(args[i], "<", app, i + 1);
        if (!real_lt(prev, cur)) return make_boolean(false);
        prev = cur;
    }
    return make_boolean(true);
}

static Value _prim_num_gt(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    NumReal prev = _num(args[0], ">", app, 1);
    for (int i = 1; i < static_cast<int>(args.size()); ++i) {
        NumReal cur = _num(args[i], ">", app, i + 1);
        if (!real_lt(cur, prev)) return make_boolean(false);
        prev = cur;
    }
    return make_boolean(true);
}

static Value _prim_num_le(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    NumReal prev = _num(args[0], "<=", app, 1);
    for (int i = 1; i < static_cast<int>(args.size()); ++i) {
        NumReal cur = _num(args[i], "<=", app, i + 1);
        if (!real_le(prev, cur)) return make_boolean(false);
        prev = cur;
    }
    return make_boolean(true);
}

static Value _prim_num_ge(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    NumReal prev = _num(args[0], ">=", app, 1);
    for (int i = 1; i < static_cast<int>(args.size()); ++i) {
        NumReal cur = _num(args[i], ">=", app, i + 1);
        if (!real_le(cur, prev)) return make_boolean(false);
        prev = cur;
    }
    return make_boolean(true);
}

void register_comparison() {
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
