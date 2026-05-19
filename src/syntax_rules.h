#pragma once
#include "value.h"
#include "environment.h"
#include "scheme_export.h"
#include <string_view>

// Parse a (syntax-rules ...) body into a SchemeSyntaxTransformer Value.
// tail is everything after the "syntax-rules" keyword (ellipsis? literals rules...).
// def_env is the environment at the define-syntax call site (may be nullptr).
// name is the macro name for error messages.
// Declared extern "C" linkage name so expander.cpp can find it without header cycle.
SCHEME_API Value parse_syntax_rules_val(Value tail,
                                         Environment* def_env,
                                         std::string_view name);

// Apply a SyntaxTransformer value to a form. Raises SchemeSyntaxError if no
// pattern matches.
SCHEME_API Value apply_syntax_transformer(Value transformer, Value form);
