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
CEKSCHEME_API const std::unordered_map<std::string, PrimitiveHelp>& primitive_help();

// Port of CATEGORY_TITLES dict.
CEKSCHEME_API const std::unordered_map<std::string, std::string>& category_titles();

// Port of CATEGORY_ORDER list.
CEKSCHEME_API const std::vector<std::string>& category_order();

// Port of register_primitive().  hi==-1 means variadic (Python None).
// Called from each module's register_xxx() function.
CEKSCHEME_API void register_primitive(
    const std::string& name, int lo, int hi,
    BuiltinFn fn,
    const std::string& usage = "",
    const std::string& doc   = "",
    const std::string& category = "",
    const std::string& kind  = "primitive");

// Port of install_primitives(env).
// Calls all module register_xxx() functions (once), then binds every
// registered primitive into env.
CEKSCHEME_API void install_primitives(Environment* env);

// ── Per-module register functions (port of each module's register()) ──────────
CEKSCHEME_API void register_control();
CEKSCHEME_API void register_lazy();
CEKSCHEME_API void register_binding();
CEKSCHEME_API void register_quotation();
CEKSCHEME_API void register_macros();
CEKSCHEME_API void register_modules();
CEKSCHEME_API void register_lists();
CEKSCHEME_API void register_arithmetic();
CEKSCHEME_API void register_comparison();
CEKSCHEME_API void register_predicates();
CEKSCHEME_API void register_equivalence();
CEKSCHEME_API void register_logical();
CEKSCHEME_API void register_meta();
CEKSCHEME_API void register_ports();
CEKSCHEME_API void register_strings();
CEKSCHEME_API void register_chars();
CEKSCHEME_API void register_vectors();
CEKSCHEME_API void register_bytevectors();
CEKSCHEME_API void register_help_sys();
CEKSCHEME_API void register_debug();
