// syntax_rules_stubs.cpp — Placeholder until syntax_rules.cpp is written.
// Replace with syntax_rules.cpp in Phase 2b.
#include "value.h"
#include "environment.h"
#include "exceptions.h"
#include <string_view>

Value parse_syntax_rules_val(Value /*tail*/, Environment* /*def_env*/,
                              std::string_view /*name*/)
   {
   throw SchemeSyntaxError("syntax-rules not yet implemented in CEKScheme C++ port");
   }

Value apply_syntax_transformer(Value /*transformer*/, Value /*form*/)
   {
   throw SchemeSyntaxError("syntax-rules macro application not yet implemented");
   }
