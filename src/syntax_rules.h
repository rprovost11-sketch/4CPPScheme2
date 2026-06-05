#pragma once
// syntax_rules.h -- R7RS syntax-rules pattern matcher and template instantiator.
// Direct port of pyscheme/syntax_rules.py.
#include "AST.h"
#include "Environment.h"
#include <string>

// Port of syntax_rules.py hygiene_gensym.
// Returns base unchanged if already a gensym (starts with \x01h. prefix).
CPPSCHEME2_API std::string hygiene_gensym(const std::string& base);

// Port of syntax_rules.py parse_syntax_rules.
// tail     - cdr of (syntax-rules ...) form
// def_env  - definition-time environment (free ids resolved here)
// name     - transformer name for error messages
// form_src - source of the whole (syntax-rules ...) form, for the
//            'malformed' diagnostic (tail may be a NIL with no position)
CPPSCHEME2_API Value parse_syntax_rules(Value tail, Environment* def_env,
                                        const std::string& name,
                                        SourceInfo* form_src = nullptr);

// Port of syntax_rules.py apply_syntax_transformer.
// Tries each rule in order; returns expanded form.
// Throws SchemeSyntaxError if no pattern matches.
CPPSCHEME2_API Value apply_syntax_transformer(const Value& t, const Value& form);
