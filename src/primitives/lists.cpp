// primitives/lists.cpp -- list and pair primitives.
// Direct port of pyscheme/primitives/lists.py.
#include "lists.h"
#include "primitives.h"
#include "equivalence.h"
#include "../AST.h"
#include "../Environment.h"
#include "../Evaluator.h"
#include "../gc.h"
#include <variant>

static const char* CATEGORY = "lists";

static SourceInfo* _src(const Value* a) { return a ? src_of(*a) : nullptr; }

static const void* _heap_ptr(const Value& v) {
    return std::visit([](auto&& x) -> const void* {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_pointer_v<T>) return static_cast<const void*>(x);
        return nullptr;
    }, v.repr);
}

static bool _eq_pred(const Value& a, const Value& b) {
    if (eqv_atom(a, b)) return true;
    const void* pa = _heap_ptr(a);
    const void* pb = _heap_ptr(b);
    return pa && pa == pb;
}

static bool _eqv_pred(const Value& a, const Value& b) { return eqv_atom(a, b); }

static bool _equal_pred(const Value& a, const Value& b) { return value_equal(a, b); }

// ── Floyd cycle detection ─────────────────────────────────────────────────────

static bool _proper_list_p(const Value& v) {
    Value slow = v;
    Value fast = v;
    while (true) {
        if (is_nil(fast))   return true;
        if (!is_cons(fast)) return false;
        fast = cdr(fast);
        if (is_nil(fast))   return true;
        if (!is_cons(fast)) return false;
        fast = cdr(fast);
        slow = cdr(slow);
        if (is_nil(fast))   return true;
        if (!is_cons(fast)) return false;
        if (!is_cons(slow)) return false;
        if (std::get<ConsCell*>(slow.repr) == std::get<ConsCell*>(fast.repr))
            return false;
    }
}

// ── Primitives ────────────────────────────────────────────────────────────────

static Value _prim_cons(Context*, Environment*, std::vector<Value>& args, const Value*) {
    return alloc_cons(args[0], args[1]);
}

static Value _prim_car(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    if (!is_cons(args[0]))
        throw SchemeTypeError("car: expected pair, got non-pair value", _src(app));
    return car(args[0]);
}

static Value _prim_cdr(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    if (!is_cons(args[0]))
        throw SchemeTypeError("cdr: expected pair, got non-pair value", _src(app));
    return cdr(args[0]);
}

static Value _prim_pair_p(Context*, Environment*, std::vector<Value>& args, const Value*) {
    return make_boolean(is_cons(args[0]));
}

static Value _prim_null_p(Context*, Environment*, std::vector<Value>& args, const Value*) {
    return make_boolean(is_nil(args[0]));
}

static Value _prim_list(Context*, Environment*, std::vector<Value>& args, const Value*) {
    Value result = NIL_VALUE;
    int i = (int)args.size() - 1;
    while (i >= 0) { result = alloc_cons(args[i], result); --i; }
    return result;
}

static Value _prim_list_p(Context*, Environment*, std::vector<Value>& args, const Value*) {
    return make_boolean(_proper_list_p(args[0]));
}

static Value _prim_length(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    Value cur = args[0];
    int64_t n = 0;
    while (is_cons(cur)) { ++n; cur = cdr(cur); }
    if (!is_nil(cur))
        throw SchemeTypeError("length: argument must be a proper list", _src(app));
    return make_integer(n);
}

static Value _prim_reverse(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    Value result = NIL_VALUE;
    Value cur = args[0];
    while (is_cons(cur)) { result = alloc_cons(car(cur), result); cur = cdr(cur); }
    if (!is_nil(cur))
        throw SchemeTypeError("reverse: argument must be a proper list", _src(app));
    return result;
}

static Value _prim_list_tail(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    if (!is_integer(args[1]))
        throw SchemeTypeError("list-tail: index must be an integer", _src(app));
    int64_t k = as_integer(args[1]);
    if (k < 0)
        throw SchemeTypeError("list-tail: index must be non-negative", _src(app));
    Value cur = args[0];
    for (int64_t i = 0; i < k; ++i) {
        if (!is_cons(cur))
            throw SchemeTypeError("list-tail: index out of range", _src(app));
        cur = cdr(cur);
    }
    return cur;
}

static Value _prim_list_ref(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    if (!is_integer(args[1]))
        throw SchemeTypeError("list-ref: index must be an integer", _src(app));
    int64_t k = as_integer(args[1]);
    if (k < 0)
        throw SchemeTypeError("list-ref: index must be non-negative", _src(app));
    Value cur = args[0];
    for (int64_t i = 0; i < k; ++i) {
        if (!is_cons(cur))
            throw SchemeTypeError("list-ref: index out of range", _src(app));
        cur = cdr(cur);
    }
    if (!is_cons(cur))
        throw SchemeTypeError("list-ref: index out of range", _src(app));
    return car(cur);
}

static Value _prim_list_copy(Context*, Environment*, std::vector<Value>& args, const Value*) {
    if (is_nil(args[0]))  return NIL_VALUE;
    if (!is_cons(args[0])) return args[0];
    std::vector<Value> collected;
    Value cur = args[0];
    while (is_cons(cur)) { collected.push_back(car(cur)); cur = cdr(cur); }
    Value tail = cur;
    int i = (int)collected.size() - 1;
    while (i >= 0) { tail = alloc_cons(collected[i], tail); --i; }
    return tail;
}

// ── member / assoc search builders ───────────────────────────────────────────

using _EqFn = bool(*)(const Value&, const Value&);

static Value _member_search(const char* name, _EqFn eq_pred,
                             Context* ctx, std::vector<Value>& args, const Value* app) {
    GcRootGuard target(args[0]);
    GcRootGuard cur(args[1]);
    if (args.size() >= 3) {
        GcRootGuard cmp_proc(args[2]);
        while (is_cons(cur.val)) {
            std::vector<Value> call_args = {target.val, car(cur.val)};
            GcRootVec call_args_root(call_args);
            Value res = apply_scheme_proc(cmp_proc.val, call_args, ctx, nullptr, app);
            if (is_truthy(res)) return cur.val;
            cur.val = cdr(cur.val);
        }
    } else {
        while (is_cons(cur.val)) {
            if (eq_pred(target.val, car(cur.val))) return cur.val;
            cur.val = cdr(cur.val);
        }
    }
    if (!is_nil(cur.val))
        throw SchemeTypeError(std::string(name) + ": second argument must be a proper list", _src(app));
    return make_boolean(false);
}

static Value _assoc_search(const char* name, _EqFn eq_pred,
                            Context* ctx, std::vector<Value>& args, const Value* app) {
    GcRootGuard target(args[0]);
    GcRootGuard cur(args[1]);
    if (args.size() >= 3) {
        GcRootGuard cmp_proc(args[2]);
        while (is_cons(cur.val)) {
            Value pair = car(cur.val);
            if (!is_cons(pair))
                throw SchemeTypeError(std::string(name) + ": alist entries must be pairs", _src(app));
            std::vector<Value> call_args = {target.val, car(pair)};
            GcRootVec call_args_root(call_args);
            Value res = apply_scheme_proc(cmp_proc.val, call_args, ctx, nullptr, app);
            if (is_truthy(res)) return pair;
            cur.val = cdr(cur.val);
        }
    } else {
        while (is_cons(cur.val)) {
            Value pair = car(cur.val);
            if (!is_cons(pair))
                throw SchemeTypeError(std::string(name) + ": alist entries must be pairs", _src(app));
            if (eq_pred(target.val, car(pair))) return pair;
            cur.val = cdr(cur.val);
        }
    }
    if (!is_nil(cur.val))
        throw SchemeTypeError(std::string(name) + ": second argument must be a proper list", _src(app));
    return make_boolean(false);
}

static Value _prim_member(Context* c, Environment*, std::vector<Value>& a, const Value* n) { return _member_search("member", _equal_pred, c, a, n); }
static Value _prim_memv  (Context* c, Environment*, std::vector<Value>& a, const Value* n) { return _member_search("memv",   _eqv_pred,   c, a, n); }
static Value _prim_memq  (Context* c, Environment*, std::vector<Value>& a, const Value* n) { return _member_search("memq",   _eq_pred,    c, a, n); }
static Value _prim_assoc (Context* c, Environment*, std::vector<Value>& a, const Value* n) { return _assoc_search ("assoc",  _equal_pred, c, a, n); }
static Value _prim_assv  (Context* c, Environment*, std::vector<Value>& a, const Value* n) { return _assoc_search ("assv",   _eqv_pred,   c, a, n); }
static Value _prim_assq  (Context* c, Environment*, std::vector<Value>& a, const Value* n) { return _assoc_search ("assq",   _eq_pred,    c, a, n); }

// ── map / for-each ────────────────────────────────────────────────────────────

static Value _prim_map(Context* ctx, Environment*, std::vector<Value>& args, const Value* app) {
    if (args.size() < 2)
        throw SchemeArityError(arity_mismatch_msg("map", 2, -1, (int)args.size()), _src(app));
    // All locals that hold Values across the apply_scheme_proc callback must
    // be registered as GC roots -- the inner cek_eval can trigger a minor GC,
    // and we own the only reachable references to these Values.
    GcRootGuard proc(args[0]);
    std::vector<Value> lists(args.begin() + 1, args.end());
    std::vector<Value> collected;
    GcRootVec lists_root(lists);
    GcRootVec collected_root(collected);
    while (true) {
        bool ready = true;
        for (auto& lst : lists) { if (!is_cons(lst)) { ready = false; break; } }
        if (!ready) break;
        std::vector<Value> row, next_lists;
        GcRootVec row_root(row);
        GcRootVec next_lists_root(next_lists);
        for (auto& lst : lists) { row.push_back(car(lst)); next_lists.push_back(cdr(lst)); }
        collected.push_back(apply_scheme_proc(proc.val, row, ctx, nullptr, app));
        lists = std::move(next_lists);
    }
    for (auto& lst : lists) {
        if (!is_nil(lst))
            throw SchemeTypeError("map: list arguments must be proper lists", _src(app));
    }
    Value result = NIL_VALUE;
    int i = (int)collected.size() - 1;
    while (i >= 0) { result = alloc_cons(collected[i], result); --i; }
    return result;
}

static Value _prim_filter(Context* ctx, Environment*, std::vector<Value>& args, const Value* app) {
    GcRootGuard pred(args[0]);
    Value lst = args[1];
    GcRootGuard lst_root(lst);
    std::vector<Value> collected;
    GcRootVec collected_root(collected);
    while (is_cons(lst)) {
        std::vector<Value> row;
        GcRootVec row_root(row);
        row.push_back(car(lst));
        Value keep = apply_scheme_proc(pred.val, row, ctx, nullptr, app);
        if (is_truthy(keep))
            collected.push_back(car(lst));
        lst = cdr(lst);
    }
    if (!is_nil(lst))
        throw SchemeTypeError("filter: list argument must be a proper list", _src(app));
    Value result = NIL_VALUE;
    int i = (int)collected.size() - 1;
    while (i >= 0) { result = alloc_cons(collected[i], result); --i; }
    return result;
}

static Value _prim_for_each(Context* ctx, Environment*, std::vector<Value>& args, const Value* app) {
    if (args.size() < 2)
        throw SchemeArityError(arity_mismatch_msg("for-each", 2, -1, (int)args.size()), _src(app));
    GcRootGuard proc(args[0]);
    std::vector<Value> lists(args.begin() + 1, args.end());
    GcRootVec lists_root(lists);
    while (true) {
        bool ready = true;
        for (auto& lst : lists) { if (!is_cons(lst)) { ready = false; break; } }
        if (!ready) break;
        std::vector<Value> row, next_lists;
        GcRootVec row_root(row);
        GcRootVec next_lists_root(next_lists);
        for (auto& lst : lists) { row.push_back(car(lst)); next_lists.push_back(cdr(lst)); }
        apply_scheme_proc(proc.val, row, ctx, nullptr, app);
        lists = std::move(next_lists);
    }
    for (auto& lst : lists) {
        if (!is_nil(lst))
            throw SchemeTypeError("for-each: list arguments must be proper lists", _src(app));
    }
    return VOID_VALUE;
}

// ── set-car! / set-cdr! ───────────────────────────────────────────────────────

static Value _prim_set_car(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    if (!is_cons(args[0]))
        throw SchemeTypeError("set-car!: argument must be a pair", _src(app));
    if (is_immutable(args[0]))
        throw SchemeTypeError("set-car!: argument is an immutable literal", _src(app));
    set_car(args[0], args[1]);
    return VOID_VALUE;
}

static Value _prim_set_cdr(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    if (!is_cons(args[0]))
        throw SchemeTypeError("set-cdr!: argument must be a pair", _src(app));
    if (is_immutable(args[0]))
        throw SchemeTypeError("set-cdr!: argument is an immutable literal", _src(app));
    set_cdr(args[0], args[1]);
    return VOID_VALUE;
}

// ── make-list / list-set! ─────────────────────────────────────────────────────

static Value _prim_make_list(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    if (!is_integer(args[0]))
        throw SchemeTypeError("make-list: length must be an integer", _src(app));
    int64_t k = as_integer(args[0]);
    if (k < 0)
        throw SchemeTypeError("make-list: length must be non-negative", _src(app));
    Value fill = args.size() >= 2 ? args[1] : VOID_VALUE;
    Value result = NIL_VALUE;
    for (int64_t i = 0; i < k; ++i) result = alloc_cons(fill, result);
    return result;
}

static Value _prim_list_set(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    if (!is_integer(args[1]))
        throw SchemeTypeError("list-set!: index must be an integer", _src(app));
    int64_t k = as_integer(args[1]);
    if (k < 0)
        throw SchemeTypeError("list-set!: index must be non-negative", _src(app));
    Value cur = args[0];
    for (int64_t i = 0; i < k; ++i) {
        if (!is_cons(cur))
            throw SchemeTypeError("list-set!: index out of range", _src(app));
        cur = cdr(cur);
    }
    if (!is_cons(cur))
        throw SchemeTypeError("list-set!: index out of range", _src(app));
    set_car(cur, args[2]);
    return VOID_VALUE;
}

// ── append ────────────────────────────────────────────────────────────────────

static Value _prim_append(Context*, Environment*, std::vector<Value>& args, const Value* app) {
    if (args.empty()) return NIL_VALUE;
    if (args.size() == 1) return args[0];
    std::vector<Value> collected;
    for (size_t idx = 0; idx < args.size() - 1; ++idx) {
        Value cur = args[idx];
        while (is_cons(cur)) { collected.push_back(car(cur)); cur = cdr(cur); }
        if (!is_nil(cur))
            throw SchemeTypeError("append: non-last argument must be a proper list", _src(app));
    }
    Value tail = args.back();
    int i = (int)collected.size() - 1;
    while (i >= 0) { tail = alloc_cons(collected[i], tail); --i; }
    return tail;
}

// ── cxr family ────────────────────────────────────────────────────────────────

static BuiltinFn _make_cxr_fn(std::string name, std::string ops) {
    return [name, ops](Context*, Environment*, std::vector<Value>& args, const Value* app) -> Value {
        Value v = args[0];
        int i = (int)ops.size() - 1;
        while (i >= 0) {
            if (!is_cons(v))
                throw SchemeTypeError(name + ": chain hit a non-pair",
                                      app ? src_of(*app) : nullptr);
            v = (ops[i] == 'a') ? car(v) : cdr(v);
            --i;
        }
        return v;
    };
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_lists() {
    register_primitive("cons",      2,  2,  _prim_cons,      "",
        "Return a newly allocated pair whose car is a and whose cdr is b.\n"
        "The pair is guaranteed to be different (in the sense of eq?) from every\n"
        "existing object.", CATEGORY);
    register_primitive("car",       1,  1,  _prim_car,       "", "Return the contents of the car field of the pair.  Signals an error if not a pair.  R7RS 6.4.", CATEGORY);
    register_primitive("cdr",       1,  1,  _prim_cdr,       "", "Return the contents of the cdr field of the pair.  Signals an error if not a pair.  R7RS 6.4.", CATEGORY);
    register_primitive("pair?",     1,  1,  _prim_pair_p,    "", "Return #t if a is a pair, #f otherwise.  The empty list is not a pair.  R7RS 6.4.", CATEGORY);
    register_primitive("null?",     1,  1,  _prim_null_p,    "", "Return #t if a is the empty list (), #f otherwise.  R7RS 6.4.", CATEGORY);
    register_primitive("list",      0, -1,  _prim_list,      "", "Return a newly allocated list containing the arguments in order.  R7RS 6.4.", CATEGORY);
    register_primitive("list?",     1,  1,  _prim_list_p,    "", "Return #t if a is a proper list (terminated by ()).  R7RS 6.4.", CATEGORY);
    register_primitive("length",    1,  1,  _prim_length,    "", "Return the number of elements in the list.  Argument must be a proper list.  R7RS 6.4.", CATEGORY);
    register_primitive("reverse",   1,  1,  _prim_reverse,   "", "Return a newly allocated list of the elements of list in reverse order.  R7RS 6.4.", CATEGORY);
    register_primitive("list-tail", 2,  2,  _prim_list_tail, "", "(list-tail list k) returns the sublist obtained by omitting the first k elements.  R7RS 6.4.", CATEGORY);
    register_primitive("list-ref",  2,  2,  _prim_list_ref,  "", "(list-ref list k) returns the kth element of list (zero-based).  R7RS 6.4.", CATEGORY);
    register_primitive("list-copy", 1,  1,  _prim_list_copy, "", "Return a newly allocated copy of the spine of the list, sharing element values.  R7RS 6.4.", CATEGORY);
    register_primitive("member",    2,  3,  _prim_member,    "", "(member obj list [compare]) returns the first sublist of list whose car is equal? to obj, or #f if none.  R7RS 6.4.", CATEGORY);
    register_primitive("memv",      2,  2,  _prim_memv,      "", "Like member but uses eqv? for comparison.  R7RS 6.4.", CATEGORY);
    register_primitive("memq",      2,  2,  _prim_memq,      "", "Like member but uses eq? for comparison.  R7RS 6.4.", CATEGORY);
    register_primitive("assoc",     2,  3,  _prim_assoc,     "", "(assoc obj alist [compare]) returns the first pair in alist whose car is equal? to obj, or #f if none.  R7RS 6.4.", CATEGORY);
    register_primitive("assv",      2,  2,  _prim_assv,      "", "Like assoc but uses eqv? for comparison.  R7RS 6.4.", CATEGORY);
    register_primitive("assq",      2,  2,  _prim_assq,      "", "Like assoc but uses eq? for comparison.  R7RS 6.4.", CATEGORY);
    register_primitive("map",       2, -1,  _prim_map,       "", "(map proc list1 list2 ...) applies proc element-wise and returns a list of results.  R7RS 6.10.", CATEGORY);
    register_primitive("for-each",  2, -1,  _prim_for_each,  "", "(for-each proc list1 list2 ...) applies proc element-wise for effect and returns unspecified.  R7RS 6.10.", CATEGORY);
    register_primitive("filter",    2,  2,  _prim_filter,    "", "(filter pred list) returns a new list of the elements of list for which pred returns a true value.  R7RS-large / SRFI-1.", CATEGORY);
    register_primitive("set-car!",  2,  2,  _prim_set_car,   "", "(set-car! pair obj) replaces the car of pair with obj.  R7RS 6.4.", CATEGORY);
    register_primitive("set-cdr!",  2,  2,  _prim_set_cdr,   "", "(set-cdr! pair obj) replaces the cdr of pair with obj.  R7RS 6.4.", CATEGORY);
    register_primitive("make-list", 1,  2,  _prim_make_list, "", "(make-list k [fill]) returns a list of length k with each element fill (default unspecified).  R7RS 6.4.", CATEGORY);
    register_primitive("list-set!", 3,  3,  _prim_list_set,  "", "(list-set! list k obj) replaces the kth element with obj.  R7RS 6.4.", CATEGORY);
    register_primitive("append",    0, -1,  _prim_append,    "", "Return a list that is the concatenation of its arguments.  All but the last must be proper lists.  R7RS 6.4.", CATEGORY);

    static const char* CXR_NAMES[] = {
        "caar",  "cadr",  "cdar",  "cddr",
        "caaar", "caadr", "cadar", "caddr",
        "cdaar", "cdadr", "cddar", "cdddr",
        "caaaar","caaadr","caadar","caaddr",
        "cadaar","cadadr","caddar","cadddr",
        "cdaaar","cdaadr","cdadar","cdaddr",
        "cddaar","cddadr","cdddar","cddddr",
    };
    for (const char* n : CXR_NAMES) {
        std::string name(n);
        std::string ops = name.substr(1, name.size() - 2);
        register_primitive(name, 1, 1, _make_cxr_fn(name, ops), "",
            "(" + name + " x) is shorthand for the corresponding car/cdr composition.  R7RS 6.4 / (scheme cxr).",
            CATEGORY);
    }
}
