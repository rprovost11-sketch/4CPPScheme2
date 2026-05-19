#pragma once
#include "value.h"
#include "environment.h"
#include "scheme_export.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <optional>

// hygiene_gensym: generate a fresh symbol name with the \x01h. prefix.
// If base already has the prefix, return it unchanged.
SCHEME_API std::string hygiene_gensym(std::string_view base);
SCHEME_API void        hygiene_gensym_reset();  // for tests

// Expand a single s-expression: desugar and alpha-rename.
// env_ref must be the current runtime environment pointer; the expander
// temporarily swaps it when processing let-syntax / letrec-syntax / define-syntax.
SCHEME_API Value expand(Value sexpr, Environment*& env_ref);

// apply_syntax_transformer: apply a SyntaxTransformer value to a form.
// Defined in syntax_rules.cpp; declared here for the expander to call.
SCHEME_API Value apply_syntax_transformer(Value transformer, Value form);
