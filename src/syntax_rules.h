#pragma once
// syntax_rules.h -- R7RS syntax-rules pattern matcher and template instantiator.
// Direct port of pyscheme/syntax_rules.py.
#include "AST.h"
#include "Environment.h"
#include <string>
#include <vector>
#include <cstdint>

// Port of syntax_rules.py hygiene_gensym.
// Returns base unchanged if already a gensym (starts with \x01h. prefix), unless
// force=true, which always produces a fresh gensym from the base's display name
// (used for per-application binders that may arrive already gensym'd -- A1f).
CPPSCHEME2_API std::string hygiene_gensym(const std::string& base,
                                          bool force = false);

// Port of syntax_rules.py formals_bound_names / let_binding_names.  The single
// binder-extractors for lambda formals and let/letrec binding lists, shared by
// both hygiene walkers -- collect_binding_intros (syntax_rules.cpp) and
// Expander's rename_refs_in_form -- so the two can't disagree on what a binder
// position binds.  Return interned symbol ids, in order.
CPPSCHEME2_API std::vector<uint32_t> formals_bound_names(const Value& formals);
CPPSCHEME2_API std::vector<uint32_t> let_binding_names(const Value& bindings);

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
