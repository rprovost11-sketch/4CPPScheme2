// primitives/primitives.cpp -- primitive registry.
// Direct port of pyscheme/primitives/__init__.py.
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"
#include "../Analyzer.h"
#include <algorithm>
#include <stdexcept>

// ── Static registry tables ────────────────────────────────────────────────────
// Port of _REGISTRY, PRIMITIVE_ARITIES, PRIMITIVE_HELP.

struct RegistryEntry {
    std::string name;
    int         lo;
    int         hi;      // -1 = variadic (Python None)
    BuiltinFn   fn;
    std::string usage;
    std::string doc;
    std::string kind;
    std::string category;
};

static std::vector<RegistryEntry>                       s_registry;
static std::unordered_map<std::string, PrimitiveHelp>   s_prim_help;
static bool s_installed = false;

// ── Category tables (port of CATEGORY_TITLES / CATEGORY_ORDER) ───────────────

static const std::unordered_map<std::string, std::string> s_category_titles = {
    { "control",     "Control"         },
    { "lazy",        "Lazy Evaluation" },
    { "binding",     "Binding"         },
    { "quotation",   "Quotation"       },
    { "macros",      "Macros"          },
    { "modules",     "Modules"         },
    { "lists",       "Lists"           },
    { "arithmetic",  "Arithmetic"      },
    { "comparison",  "Comparison"      },
    { "predicates",  "Predicates"      },
    { "equivalence", "Equivalence"     },
    { "logical",     "Logical"         },
    { "meta",        "Meta"            },
    { "help_sys",    "Help"            },
    { "debug",       "Debugging"       },
};

static const std::vector<std::string> s_category_order = {
    "control", "lazy", "binding", "quotation", "macros", "modules",
    "lists", "arithmetic", "comparison", "predicates",
    "equivalence", "logical", "meta", "ports",
    "strings", "chars", "vectors", "bytevectors",
    "help_sys", "debug",
};

// ── _default_usage (port of __init__.py _default_usage) ──────────────────────

static std::string _default_usage(const std::string& name, int lo, int hi) {
    static const char stock[] = "abcdefgh";
    std::vector<std::string> fixed;
    for (int i = 0; i < lo; ++i) {
        if (i < 8) fixed.push_back(std::string(1, stock[i]));
        else       fixed.push_back("arg" + std::to_string(i + 1));
    }
    if (hi == -1) {                          // variadic
        if (lo == 0) return "(" + name + " . args)";
        std::string r = "(" + name;
        for (auto& s : fixed) r += " " + s;
        return r + " . rest)";
    }
    if (lo == hi) {
        if (lo == 0) return "(" + name + ")";
        std::string r = "(" + name;
        for (auto& s : fixed) r += " " + s;
        return r + ")";
    }
    // optional args: fixed + [argN] for lo..hi
    std::string r = "(" + name;
    for (auto& s : fixed) r += " " + s;
    for (int i = lo; i < hi; ++i)
        r += " [arg" + std::to_string(i + 1) + "]";
    return r + ")";
}

// ── _wrap_arity (port of __init__.py _wrap_arity) ────────────────────────────

static BuiltinFn _wrap_arity(const std::string& name, int lo, int hi, BuiltinFn fn) {
    return [name, lo, hi, fn](Context* ctx, Environment* env,
                               std::vector<Value>& args, const Value* app_node) -> Value {
        int n = static_cast<int>(args.size());
        auto src = app_node ? src_of(*app_node) : nullptr;
        if (n < lo) throw SchemeArityError(arity_mismatch_msg(name, lo, hi, n), src);
        if (hi != -1 && n > hi) throw SchemeArityError(arity_mismatch_msg(name, lo, hi, n), src);
        return fn(ctx, env, args, app_node);
    };
}

// ── Public API ────────────────────────────────────────────────────────────────

const std::unordered_map<std::string, PrimitiveHelp>& primitive_help() {
    return s_prim_help;
}

const std::unordered_map<std::string, std::string>& category_titles() {
    return s_category_titles;
}

const std::vector<std::string>& category_order() {
    return s_category_order;
}

void register_primitive(
    const std::string& name, int lo, int hi,
    BuiltinFn fn,
    const std::string& usage,
    const std::string& doc,
    const std::string& category,
    const std::string& kind)
{
    std::string u = usage.empty() ? _default_usage(name, lo, hi) : usage;
    BuiltinFn wrapped = _wrap_arity(name, lo, hi, fn);
    s_registry.push_back({ name, lo, hi, wrapped, u, doc, kind, category });
    s_prim_help[name] = { kind, u, doc, category };
    // Port of Python: PRIMITIVE_ARITIES[name] = arity
    register_primitive_arity(name, lo, hi);
}

void install_primitives(Environment* env) {
    if (!s_installed) {
        init_value_pools();
        // Port of __init__.py module-level register() calls.
        register_control();
        register_lazy();
        register_binding();
        register_quotation();
        register_macros();
        register_modules();
        register_lists();
        register_arithmetic();
        register_comparison();
        register_predicates();
        register_equivalence();
        register_logical();
        register_meta();
        register_ports();
        register_strings();
        register_chars();
        register_vectors();
        register_bytevectors();
        register_help_sys();
        register_debug();
        s_installed = true;
    }
    for (auto& e : s_registry)
        env->bind(e.name, make_primitive(e.name, e.fn));
}
