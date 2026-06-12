#pragma once
// primitives/primitives.h -- primitive registry.
// Direct port of pyscheme/primitives/__init__.py.
#include "../AST.h"
#include "../Environment.h"
#include "../scheme_export.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>
#include <cstdint>

// Port of __init__.py PrimitiveHelp entry: (kind, usage, doc, category).
struct PrimitiveHelp
   {
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
    const std::string& doc = "",
    const std::string& category = "",
    const std::string& kind = "primitive");

// Port of install_primitives(env).
// Calls all module register_xxx() functions (once), then binds every
// registered primitive into env.
CPPSCHEME2_API void install_primitives(Environment* env);

// Port of __init__.py parse_start_end: parse the optional [start [end]]
// arguments at args[base_idx] / args[base_idx+1] for a sequence slice,
// defaulting to [0, length).  Validates each given bound is an integer and that
// 0 <= start <= end <= length, then returns {start, end}.  range_msg is the
// out-of-range error text -- it differs per primitive and is test-pinned.
CPPSCHEME2_API std::pair<int64_t, int64_t> parse_start_end(
    const std::vector<Value>& args, size_t base_idx, int64_t length,
    const char* name, const Value* app,
    const char* range_msg = "start/end out of range");

// Port of __init__.py _check_index: validate that v is an integer index in
// [0, length) and return it, else throw "<name>: index must be an integer" or
// "<name>: index <k> out of range".  Shared by string-ref / vector-ref / set! /
// bytevector-u8-ref / set!.
CPPSCHEME2_API int64_t check_index(const Value& v, const char* name,
                                   int64_t length, const Value* app);

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
