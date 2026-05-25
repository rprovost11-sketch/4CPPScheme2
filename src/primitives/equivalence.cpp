// primitives/equivalence.cpp -- equivalence primitives.
// Direct port of pyscheme/primitives/equivalence.py.
#include "equivalence.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"
#include <unordered_set>
#include <utility>

static const char* CATEGORY = "equivalence";

// ── _value_equal: cycle-safe structural equality ──────────────────────────────
// Port of equivalence.py _value_equal.
// 'seen' tracks (ptr-a, ptr-b) pairs for mutable heap structures (cons, vector)
// to bound recursion on circular structures.

struct _PairHash {
    size_t operator()(const std::pair<const void*, const void*>& p) const {
        auto h = std::hash<const void*>{};
        return h(p.first) ^ (h(p.second) * 0x9e3779b9ULL);
    }
};
using _Seen = std::unordered_set<std::pair<const void*, const void*>, _PairHash>;

static bool _val_eq(const Value& a, const Value& b, _Seen& seen);

static bool _val_eq(const Value& a, const Value& b, _Seen& seen) {
    if (is_cons(a)) {
        if (!is_cons(b)) return false;
        auto* ca = std::get<ConsCell*>(a.repr);
        auto* cb = std::get<ConsCell*>(b.repr);
        auto key = std::make_pair((const void*)ca, (const void*)cb);
        if (seen.count(key)) return true;
        seen.insert(key);
        return _val_eq(car(a), car(b), seen) && _val_eq(cdr(a), cdr(b), seen);
    }
    if (is_cons(b)) return false;
    if (is_vector(a)) {
        if (!is_vector(b)) return false;
        const auto& va = as_vector_items_const(a);
        const auto& vb = as_vector_items_const(b);
        if (va.size() != vb.size()) return false;
        auto* pva = std::get<SchemeVector*>(a.repr);
        auto* pvb = std::get<SchemeVector*>(b.repr);
        auto key = std::make_pair((const void*)pva, (const void*)pvb);
        if (seen.count(key)) return true;
        seen.insert(key);
        for (size_t i = 0; i < va.size(); ++i)
            if (!_val_eq(va[i], vb[i], seen)) return false;
        return true;
    }
    if (is_vector(b)) return false;
    if (is_bytevector(a)) {
        if (!is_bytevector(b)) return false;
        return as_bytevector_items_const(a) == as_bytevector_items_const(b);
    }
    if (is_bytevector(b)) return false;
    // Port of Python: if a is b: return True
    {
        auto get_ptr = [](const Value& v) -> const void* {
            return std::visit([](auto&& x) -> const void* {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_pointer_v<T>) return static_cast<const void*>(x);
                return nullptr;
            }, v.repr);
        };
        const void* pa = get_ptr(a);
        const void* pb = get_ptr(b);
        if (pa && pb && pa == pb) return true;
    }
    // Atoms
    if (is_nil(a)       && is_nil(b))       return true;
    if (is_void(a)      && is_void(b))      return true;
    if (is_eof(a)       && is_eof(b))       return true;
    if (is_symbol(a)    && is_symbol(b))    return as_symbol_id(a) == as_symbol_id(b);
    if (is_integer(a)   && is_integer(b))   return as_integer(a) == as_integer(b);
    if (is_real(a)      && is_real(b))      return as_real(a) == as_real(b);
    if (is_rational(a)  && is_rational(b))
        return as_rational_num(a) == as_rational_num(b) && as_rational_den(a) == as_rational_den(b);
    if (is_complex(a)   && is_complex(b))
        return as_complex_real(a) == as_complex_real(b) && as_complex_imag(a) == as_complex_imag(b);
    if (is_exact_complex(a) && is_exact_complex(b))
        return _val_eq(as_exact_complex_real(a), as_exact_complex_real(b), seen) &&
               _val_eq(as_exact_complex_imag(a), as_exact_complex_imag(b), seen);
    if (is_boolean(a)   && is_boolean(b))   return as_boolean(a) == as_boolean(b);
    if (is_string(a)    && is_string(b))    return as_string(a) == as_string(b);
    if (is_character(a) && is_character(b)) return as_character(a) == as_character(b);
    return false;
}

bool value_equal(const Value& a, const Value& b) {
    _Seen seen;
    return _val_eq(a, b, seen);
}

// ── eq? ───────────────────────────────────────────────────────────────────────
// Port of equivalence.py _prim_eq_p.

static Value _prim_eq_p(Context*, Environment*, std::vector<Value>& args, const Value*) {
    const Value& a = args[0];
    const Value& b = args[1];
    if (is_symbol(a)  && is_symbol(b))  return make_boolean(as_symbol_id(a) == as_symbol_id(b));
    if (is_boolean(a) && is_boolean(b)) return make_boolean(as_boolean(a) == as_boolean(b));
    if (is_nil(a)     && is_nil(b))     return make_boolean(true);
    if (is_void(a)    && is_void(b))    return make_boolean(true);
    if (is_eof(a)     && is_eof(b))     return make_boolean(true);
    // All remaining types (numbers, chars, heap objects): pointer identity.
    // SchemeInteger/Real/Char are pool-allocated heap objects, so eq? on two
    // lookups of the same binding returns #t (same pointer); distinct allocations
    // return #f.  Heap objects (pairs, strings, etc.) work the same way.
    auto ptr = [](const Value& v) -> const void* {
        return std::visit([](auto&& x) -> const void* {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_pointer_v<T>) return static_cast<const void*>(x);
            return nullptr;
        }, v.repr);
    };
    const void* pa = ptr(a);
    const void* pb = ptr(b);
    if (pa && pb) return make_boolean(pa == pb);
    return make_boolean(false);
}

// ── eqv? ─────────────────────────────────────────────────────────────────────
// Port of equivalence.py _prim_eqv_p.

static Value _prim_eqv_p(Context*, Environment*, std::vector<Value>& args, const Value*) {
    return make_boolean(eqv_atom(args[0], args[1]));
}

// ── equal? ───────────────────────────────────────────────────────────────────

static Value _prim_equal_p(Context*, Environment*, std::vector<Value>& args, const Value*) {
    return make_boolean(value_equal(args[0], args[1]));
}

// ── boolean=? ────────────────────────────────────────────────────────────────

static Value _prim_boolean_eq_p(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    SourceInfo* src = app ? src_of(*app) : nullptr;
    if (!is_boolean(args[0]))
        throw SchemeTypeError("boolean=?: arguments must be booleans", src);
    bool first = as_boolean(args[0]);
    for (int i = 1; i < static_cast<int>(args.size()); ++i) {
        if (!is_boolean(args[i]))
            throw SchemeTypeError("boolean=?: arguments must be booleans", src);
        if (as_boolean(args[i]) != first) return make_boolean(false);
    }
    return make_boolean(true);
}

// ── symbol=? ─────────────────────────────────────────────────────────────────

static Value _prim_symbol_eq_p(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    SourceInfo* src = app ? src_of(*app) : nullptr;
    if (!is_symbol(args[0]))
        throw SchemeTypeError("symbol=?: arguments must be symbols", src);
    uint32_t first = as_symbol_id(args[0]);
    for (int i = 1; i < static_cast<int>(args.size()); ++i) {
        if (!is_symbol(args[i]))
            throw SchemeTypeError("symbol=?: arguments must be symbols", src);
        if (as_symbol_id(args[i]) != first) return make_boolean(false);
    }
    return make_boolean(true);
}

void register_equivalence() {
    register_primitive("eq?", 2, 2, _prim_eq_p,
        "",
        "Return #t if a and b are the same object.  Symbols with the same\n"
        "name, booleans with the same truth value, and empty lists are always\n"
        "eq?.  For pairs, strings, and vectors eq? tests allocation identity.",
        CATEGORY);

    register_primitive("eqv?", 2, 2, _prim_eqv_p,
        "",
        "Like eq?, but also returns #t for numbers with the same value and\n"
        "exactness.  (eqv? 2 2) is #t; (eqv? 2 2.0) is #f.",
        CATEGORY);

    register_primitive("equal?", 2, 2, _prim_equal_p,
        "",
        "Recursive structural equality.  Two pairs are equal? if their cars\n"
        "and cdrs are equal?; two atoms are equal? if their tag+payload match\n"
        "(ignoring source position).",
        CATEGORY);

    register_primitive("boolean=?", 2, -1, _prim_boolean_eq_p,
        "",
        "(boolean=? a b ...) returns #t when all arguments are booleans "
        "with the same truth value.  R7RS 6.3.",
        CATEGORY);

    register_primitive("symbol=?", 2, -1, _prim_symbol_eq_p,
        "",
        "(symbol=? a b ...) returns #t when all arguments are symbols "
        "with the same name.  R7RS 6.5.",
        CATEGORY);
}
