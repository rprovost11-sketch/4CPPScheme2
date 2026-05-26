#pragma once
// primitives/equivalence.h -- equivalence primitives.
// Direct port of pyscheme/primitives/equivalence.py.
#include "../AST.h"
#include "../scheme_export.h"

// Port of equivalence.py _value_equal: cycle-safe structural equality.
// Used by lists.cpp (member, assoc, equal? in list context).
CPPSCHEME2_API bool value_equal(const Value& a, const Value& b);
