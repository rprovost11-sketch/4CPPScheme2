// primitives/macros.cpp -- macro definition special form stubs.
// Direct port of pyscheme/primitives/macros.py.
#include "macros.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"

static const char* CATEGORY = "macros";
static const char* SPECIAL = "special";

static SourceInfo* _src(const Value* app_node)
   {
   return app_node ? src_of(*app_node) : nullptr;
   }

static Value _stub(const char* name, const Value* app_node)
   {
   throw SchemeTypeError(
       std::string("'") + name + "' is a special form, not a procedure; it cannot be "
                                 "applied as a first-class value.  This stub exists only to carry "
                                 "documentation into the help system.",
       _src(app_node));
   }

static Value _form_define_syntax(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   return _stub("define-syntax", a);
   }
static Value _form_let_syntax(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   return _stub("let-syntax", a);
   }
static Value _form_letrec_syntax(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   return _stub("letrec-syntax", a);
   }
static Value _form_syntax_rules(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   return _stub("syntax-rules", a);
   }

void register_macros()
   {
   register_primitive("define-syntax", 2, 2, _form_define_syntax,
                      "(define-syntax <name> <transformer>)",
                      "Bind <name> to a syntax transformer at expand time.  <transformer>\n"
                      "must be a (syntax-rules ...) form.  Subsequent occurrences of\n"
                      "<name> in head position will be expanded by the transformer.\n"
                      "R7RS 4.3.",
                      CATEGORY, SPECIAL);

   register_primitive("let-syntax", 2, -1, _form_let_syntax,
                      "(let-syntax ((<name> <transformer>)...) <body>...)",
                      "Locally bind one or more syntax transformers for the duration\n"
                      "of <body>.  Each transformer's definition env is the enclosing\n"
                      "scope (siblings are NOT visible to each other).  R7RS 4.3.",
                      CATEGORY, SPECIAL);

   register_primitive("letrec-syntax", 2, -1, _form_letrec_syntax,
                      "(letrec-syntax ((<name> <transformer>)...) <body>...)",
                      "Like let-syntax, but each transformer's definition env includes\n"
                      "its sibling transformers, so mutually recursive macros can be\n"
                      "defined in one form.  R7RS 4.3.",
                      CATEGORY, SPECIAL);

   register_primitive("syntax-rules", 1, -1, _form_syntax_rules,
                      "(syntax-rules [<ellipsis>] (<literal>...) (<pattern> <template>)...)",
                      "Create a syntax transformer.  Each <pattern> matches the use-site\n"
                      "form structurally; the first matching rule's <template> is\n"
                      "substituted with captured pattern variables.  Literals match\n"
                      "themselves; '...' repeats the preceding pattern.  R7RS 4.3.",
                      CATEGORY, SPECIAL);
   }
