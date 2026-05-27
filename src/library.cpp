// library.cpp -- R7RS library system (5.6): define-library, import, export.
// Direct port of pyscheme/library.py.
#include "library.h"
#include "gc.h"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// Module-level library registry.  Populated at startup by
// register_standard_libraries and at runtime by define-library.
static std::unordered_map<std::string, Environment*> _LIBRARY_REGISTRY;


std::string library_name_to_key(const Value& name_sexpr) {
    std::string result;
    Value cur = name_sexpr;
    bool first = true;
    while (is_cons(cur)) {
        Value part = car(cur);
        if (!first)
            result += '.';
        first = false;
        if (is_symbol(part))
            result += as_symbol(part);
        else if (is_integer(part))
            result += std::to_string(as_integer(part));
        else
            throw std::runtime_error(
                "library name parts must be symbols or integers");
        cur = cdr(cur);
    }
    if (!is_nil(cur))
        throw std::runtime_error("library name must be a proper list");
    if (first)
        throw std::runtime_error("empty library name");
    return result;
}


void library_register(const std::string& key, Environment* exports_env) {
    _LIBRARY_REGISTRY[key] = exports_env;
}


Environment* library_lookup(const std::string& key) {
    auto it = _LIBRARY_REGISTRY.find(key);
    if (it == _LIBRARY_REGISTRY.end())
        return nullptr;
    return it->second;
}


bool library_registered_p(const std::string& key) {
    return _LIBRARY_REGISTRY.count(key) > 0;
}


static std::string _render_name(const Value& name_sexpr) {
    std::string result = "(";
    Value cur = name_sexpr;
    bool first = true;
    while (is_cons(cur)) {
        Value part = car(cur);
        if (!first)
            result += ' ';
        first = false;
        if (is_symbol(part))
            result += as_symbol(part);
        else if (is_integer(part))
            result += std::to_string(as_integer(part));
        else
            result += '?';
        cur = cdr(cur);
    }
    result += ')';
    return result;
}


std::unordered_map<std::string, Value> resolve_import_set(const Value& import_set) {
    if (!is_cons(import_set))
        throw std::runtime_error("import set must be a list");

    Value head = car(import_set);
    Value tail = cdr(import_set);

    if (is_symbol(head)) {
        std::string name = as_symbol(head);

        if (name == "only") {
            if (!is_cons(tail))
                throw std::runtime_error("malformed 'only' import set");
            auto base = resolve_import_set(car(tail));
            std::unordered_map<std::string, Value> result;
            Value names_cur = cdr(tail);
            while (is_cons(names_cur)) {
                if (!is_symbol(car(names_cur)))
                    throw std::runtime_error("'only' filter must be a symbol");
                std::string n = as_symbol(car(names_cur));
                auto it = base.find(n);
                if (it == base.end())
                    throw std::runtime_error(
                        "'only' references name not in library: " + n);
                result[n] = it->second;
                names_cur = cdr(names_cur);
            }
            return result;
        }

        if (name == "except") {
            if (!is_cons(tail))
                throw std::runtime_error("malformed 'except' import set");
            auto result = resolve_import_set(car(tail));
            Value names_cur = cdr(tail);
            while (is_cons(names_cur)) {
                if (!is_symbol(car(names_cur)))
                    throw std::runtime_error("'except' filter must be a symbol");
                result.erase(as_symbol(car(names_cur)));
                names_cur = cdr(names_cur);
            }
            return result;
        }

        if (name == "rename") {
            if (!is_cons(tail))
                throw std::runtime_error("malformed 'rename' import set");
            auto base = resolve_import_set(car(tail));
            auto result = base;
            Value pairs_cur = cdr(tail);
            while (is_cons(pairs_cur)) {
                Value pair = car(pairs_cur);
                pairs_cur = cdr(pairs_cur);
                if (!is_cons(pair) || !is_symbol(car(pair))
                        || !is_cons(cdr(pair)) || !is_symbol(car(cdr(pair))))
                    throw std::runtime_error(
                        "'rename' pair must be (old-name new-name)");
                std::string old_name = as_symbol(car(pair));
                std::string new_name = as_symbol(car(cdr(pair)));
                auto it = base.find(old_name);
                if (it == base.end())
                    throw std::runtime_error(
                        "'rename' references name not in library: " + old_name);
                result.erase(old_name);
                result[new_name] = it->second;
            }
            return result;
        }

        if (name == "prefix") {
            if (!is_cons(tail) || !is_cons(cdr(tail))
                    || !is_symbol(car(cdr(tail))))
                throw std::runtime_error(
                    "'prefix' must be (prefix <import-set> <symbol>)");
            auto base = resolve_import_set(car(tail));
            std::string pfx = as_symbol(car(cdr(tail)));
            std::unordered_map<std::string, Value> result;
            for (const auto& [n, val] : base)
                result[pfx + n] = val;
            return result;
        }
    }

    // Plain library name: look up in registry.
    std::string key = library_name_to_key(import_set);
    Environment* exports_env = library_lookup(key);
    if (exports_env == nullptr)
        throw std::runtime_error(
            "library not found: " + _render_name(import_set));

    std::unordered_map<std::string, Value> result;
    for (const auto& [sid, val] : exports_env->_bindings)
        result[symbol_name(sid)] = val;
    return result;
}


static void _register_filtered(Environment* global_env,
                                const std::string& key,
                                const std::vector<std::string>& names) {
    Environment* exports_env = gc_alloc_environment(nullptr);
    size_t i = 0;
    while (i < names.size()) {
        const std::string& n = names[i];
        if (global_env->_bindings.count(intern_symbol(n)) > 0)
            exports_env->bind(n, global_env->lookup(n));
        i = i + 1;
    }
    exports_env->freeze();
    library_register(key, exports_env);
}


// R7RS (scheme base) procedures.  Syntax forms are not listed here --
// they live in the evaluator / expander.  Unimplemented names are
// silently skipped by _register_filtered.
static const std::vector<std::string> _SCHEME_BASE_NAMES = {
    // Arithmetic
    "*", "+", "-", "/", "<", "<=", "=", ">", ">=",
    "abs", "ceiling", "denominator", "exact", "exact-integer?", "exact?",
    "expt", "floor", "floor-quotient", "floor-remainder", "floor/",
    "gcd", "inexact", "inexact?", "integer?", "lcm", "max", "min",
    "modulo", "negative?", "number->string", "number?", "numerator",
    "odd?", "even?", "positive?", "quotient", "rational?", "real?",
    "remainder", "round", "square", "string->number",
    "truncate", "truncate-quotient", "truncate-remainder", "truncate/",
    "zero?",
    // Booleans
    "boolean=?", "boolean?", "not",
    // Pairs and lists
    "append", "assoc", "assq", "assv",
    "caar", "cadr", "cdar", "cddr",
    "car", "cdr", "cons",
    "for-each", "length", "list", "list->string", "list->vector",
    "list-copy", "list-ref", "list-set!", "list-tail", "list?",
    "make-list", "map", "member", "memq", "memv",
    "null?", "pair?", "reverse", "set-car!", "set-cdr!",
    // Strings
    "make-string", "string", "string->list", "string->symbol",
    "string->vector", "string-append", "string-copy", "string-copy!",
    "string-fill!", "string-for-each", "string-length", "string-map",
    "string-ref", "string-set!",
    "string<=?", "string<?", "string=?", "string>=?", "string>?", "string?",
    "substring", "symbol->string", "symbol=?", "symbol?",
    // Vectors
    "list->vector", "make-vector", "vector", "vector->list", "vector->string",
    "vector-append", "vector-copy", "vector-copy!", "vector-fill!",
    "vector-for-each", "vector-length", "vector-map",
    "vector-ref", "vector-set!", "vector?",
    // Control
    "apply", "call-with-values", "dynamic-wind", "values",
    "call-with-current-continuation", "call/cc",
    // Predicates
    "procedure?",
    // Parameters
    "make-parameter",
    // Equality
    "eq?", "equal?", "eqv?",
    // Errors and exceptions
    "error", "error-object-irritants", "error-object-message", "error-object?",
    "raise", "raise-continuable", "with-exception-handler",
    // Help
    "help", "apropos",
};

static const std::vector<std::string> _SCHEME_R5RS_NAMES = {
    "*", "+", "-", "/", "<", "<=", "=", ">", ">=",
    "abs", "append", "apply", "assoc", "assq", "assv",
    "boolean?", "caar", "cadr", "cdar", "cddr",
    "exact->inexact", "inexact->exact",
    "car", "cdr", "cons",
    "denominator", "dynamic-wind",
    "eq?", "equal?", "eqv?",
    "error", "eval", "even?", "expt",
    "floor", "for-each", "force",
    "gcd",
    "integer?",
    "lcm", "length", "list", "list-ref", "list-tail", "list?",
    "make-vector", "map", "max", "member", "memq", "memv", "min", "modulo",
    "negative?", "not", "null-environment", "null?", "number?", "numerator",
    "odd?",
    "pair?", "positive?", "procedure?",
    "quotient", "rational?", "real?", "remainder", "reverse", "round",
    "scheme-report-environment",
    "set-car!", "set-cdr!",
    "symbol?",
    "truncate", "values", "call-with-values",
    "vector", "vector->list", "vector-length", "vector-ref", "vector-set!",
    "vector?",
    "with-exception-handler",
    "zero?",
};

static const std::vector<std::string> _SCHEME_INEXACT_NAMES = {
    "acos", "asin", "atan", "cos", "exp",
    "finite?", "infinite?", "log", "nan?",
    "sin", "sqrt", "tan",
    "floor", "ceiling", "truncate", "round", "exact", "inexact",
};

static const std::vector<std::string> _SCHEME_COMPLEX_NAMES = {
    "angle", "imag-part", "magnitude", "make-polar", "make-rectangular",
    "real-part",
};

static const std::vector<std::string> _SCHEME_CHAR_NAMES = {
    "char-alphabetic?", "char-ci<=?", "char-ci<?", "char-ci=?",
    "char-ci>=?", "char-ci>?", "char-downcase", "char-foldcase",
    "char-lower-case?", "char-numeric?", "char-upcase", "char-upper-case?",
    "char-whitespace?", "digit-value",
    "string-ci<=?", "string-ci<?", "string-ci=?", "string-ci>=?", "string-ci>?",
    "string-downcase", "string-foldcase", "string-upcase",
};

static const std::vector<std::string> _SCHEME_FILE_NAMES = {
    "call-with-input-file", "call-with-output-file",
    "delete-file", "file-exists?", "rename-file",
    "open-binary-input-file", "open-binary-output-file",
    "open-input-file", "open-output-file",
    "with-input-from-file", "with-output-to-file",
};

static const std::vector<std::string> _SRFI_39_NAMES = {
    "make-parameter", "parameter?",
};

static const std::vector<std::string> _SCHEME_CXR_NAMES = {
    "caaar", "caadr", "cadar", "caddr",
    "cdaar", "cdadr", "cddar", "cdddr",
    "caaaar", "caaadr", "caadar", "caaddr",
    "cadaar", "cadadr", "caddar", "cadddr",
    "cdaaar", "cdaadr", "cdadar", "cdaddr",
    "cddaar", "cddadr", "cdddar", "cddddr",
};


void register_standard_libraries(Environment* global_env) {
    static bool s_hook_registered = false;
    if (!s_hook_registered) {
        s_hook_registered = true;
        gc_push_trace_hook([]() {
            for (auto& [key, env] : _LIBRARY_REGISTRY)
                if (env) gc_trace_environment(env);
        });
    }

    _register_filtered(global_env, "scheme.base", _SCHEME_BASE_NAMES);
    _register_filtered(global_env, "scheme.lazy",
        {"force", "make-promise", "promise?"});
    _register_filtered(global_env, "scheme.eval", {"eval", "environment"});
    // Empty env registration: library's forms are special syntax handled by
    // the evaluator / expander, so there are no procedures to export, but
    // import should still succeed.
    Environment* _empty_lib = gc_alloc_environment(nullptr);
    _empty_lib->freeze();
    library_register("scheme.case-lambda", _empty_lib);
    _register_filtered(global_env, "scheme.r5rs",    _SCHEME_R5RS_NAMES);
    _register_filtered(global_env, "scheme.inexact",  _SCHEME_INEXACT_NAMES);
    _register_filtered(global_env, "scheme.complex",  _SCHEME_COMPLEX_NAMES);
    _register_filtered(global_env, "scheme.char",     _SCHEME_CHAR_NAMES);
    _register_filtered(global_env, "scheme.write",
        {"display", "newline", "write", "write-char", "write-shared", "write-simple"});
    _register_filtered(global_env, "scheme.read",  {"read"});
    _register_filtered(global_env, "scheme.load",  {"load"});
    _register_filtered(global_env, "scheme.file",  _SCHEME_FILE_NAMES);
    _register_filtered(global_env, "scheme.repl",  {"interaction-environment"});
    _register_filtered(global_env, "scheme.process-context",
        {"command-line", "emergency-exit", "exit",
         "get-environment-variable", "get-environment-variables"});
    _register_filtered(global_env, "scheme.time",
        {"current-jiffy", "current-second", "jiffies-per-second"});
    _register_filtered(global_env, "scheme.cxr",  _SCHEME_CXR_NAMES);
    _register_filtered(global_env, "srfi.39",      _SRFI_39_NAMES);
}
