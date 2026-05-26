#pragma once
// primitives/primitives.h -- primitive registry.
// Direct port of pyscheme/primitives/__init__.py.
#include "../AST.h"
#include "../Environment.h"
#include "../scheme_export.h"
#include <string>
#include <unordered_map>
#include <vector>

// Port of __init__.py PrimitiveHelp entry: (kind, usage, doc, category).
struct PrimitiveHelp {
    std::string kind;
    std::string usage;
    std::string doc;
    std::string category;
};

// Port of PRIMITIVE_HELP dict.
CPPSCHEME2_API const std::unordered_map<std::string, PrimitiveHelp>& primitive_help();

// Port of CATEGORY_TITLES dict.
CPPSCHEME2_API const std::unordered_map<std::string, std::string>& category_titles();

// Port of CATEGORY_ORDER list.
CPPSCHEME2_API const std::vector<std::string>& category_order();

// Port of register_primitive().  hi==-1 means variadic (Python None).
// Called from each module's register_xxx() function.
CPPSCHEME2_API void register_primitive(
    const std::string& name, int lo, int hi,
    BuiltinFn fn,
    const std::string& usage = "",
    const std::string& doc   = "",
    const std::string& category = "",
    const std::string& kind  = "primitive");

// Port of install_primitives(env).
// Calls all module register_xxx() functions (once), then binds every
// registered primitive into env.
CPPSCHEME2_API void install_primitives(Environment* env);

// ── Per-module register functions (port of each module's register()) ──────────
CPPSCHEME2_API void register_control();
CPPSCHEME2_API void register_lazy();
CPPSCHEME2_API void register_binding();
CPPSCHEME2_API void register_quotation();
CPPSCHEME2_API void register_macros();
CPPSCHEME2_API void register_modules();
CPPSCHEME2_API void register_lists();
CPPSCHEME2_API void register_arithmetic();
CPPSCHEME2_API void register_comparison();
CPPSCHEME2_API void register_predicates();
CPPSCHEME2_API void register_equivalence();
CPPSCHEME2_API void register_logical();
CPPSCHEME2_API void register_meta();
CPPSCHEME2_API void register_ports();
CPPSCHEME2_API void register_strings();
CPPSCHEME2_API void register_chars();
CPPSCHEME2_API void register_vectors();
CPPSCHEME2_API void register_bytevectors();
CPPSCHEME2_API void register_help_sys();
CPPSCHEME2_API void register_debug();
