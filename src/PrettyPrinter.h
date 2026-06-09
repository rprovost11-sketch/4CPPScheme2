#pragma once
// PrettyPrinter.h -- Scheme value renderer.
// Direct port of pyscheme/PrettyPrinter.py.
#include "AST.h"
#include <functional>

// Port of PrettyPrinter.py _render_structure.
// Iteratively render an acyclic cons/vector structure to a string using an
// explicit task stack, so depth is heap-bounded (no C-stack recursion on the
// car-chain or vector nesting).  render_leaf renders any non-cons/non-vector
// value; write and display differ only in that leaf renderer (nested cons/vector
// are expanded here, so elements inherit the caller's mode).  The caller must
// ensure val is acyclic (cyclic values route to scheme_pretty_print_shared).
CPPSCHEME2_API std::string scheme_render_structure(
    const Value& v, const std::function<std::string(const Value&)>& render_leaf);

// Port of PrettyPrinter.py pretty_print.
// Renders a CEK value as Scheme surface syntax.
CPPSCHEME2_API std::string scheme_pretty_print(const Value& v);

// Port of PrettyPrinter.py pretty_print_shared.
// Like scheme_pretty_print but uses #n=/#n# datum labels for shared structure
// (write-shared, R7RS §6.13).
CPPSCHEME2_API std::string scheme_pretty_print_shared(const Value& v);

// Port of PrettyPrinter.py _has_cycle.
// Returns true if val contains any reference cycle in its cons/vector graph.
CPPSCHEME2_API bool scheme_has_cycle(const Value& v);
