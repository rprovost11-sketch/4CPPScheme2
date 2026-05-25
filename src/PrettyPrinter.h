#pragma once
// PrettyPrinter.h -- Scheme value renderer.
// Direct port of pyscheme/PrettyPrinter.py.
#include "AST.h"

// Port of PrettyPrinter.py pretty_print.
// Renders a CEK value as Scheme surface syntax.
CEKSCHEME_API std::string scheme_pretty_print(const Value& v);

// Port of PrettyPrinter.py pretty_print_shared.
// Like scheme_pretty_print but uses #n=/#n# datum labels for shared structure
// (write-shared, R7RS §6.13).
CEKSCHEME_API std::string scheme_pretty_print_shared(const Value& v);

// Port of PrettyPrinter.py _has_cycle.
// Returns true if val contains any reference cycle in its cons/vector graph.
CEKSCHEME_API bool scheme_has_cycle(const Value& v);
