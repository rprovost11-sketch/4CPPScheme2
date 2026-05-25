// primitives/modules.cpp -- module/library special form stubs.
// Direct port of pyscheme/primitives/modules.py.
#include "modules.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"

static const char* CATEGORY = "modules";
static const char* SPECIAL  = "special";

static SourceInfo* _src(const Value* app_node) {
    return app_node ? src_of(*app_node) : nullptr;
}

static Value _stub(const char* name, const Value* app_node) {
    throw SchemeTypeError(
        std::string("'") + name + "' is a special form, not a procedure; it cannot be "
        "applied as a first-class value.  This stub exists only to carry "
        "documentation into the help system.", _src(app_node));
}

static Value _form_include(Context*, Environment*, std::vector<Value>&, const Value* a)        { return _stub("include", a); }
static Value _form_include_ci(Context*, Environment*, std::vector<Value>&, const Value* a)     { return _stub("include-ci", a); }
static Value _form_cond_expand(Context*, Environment*, std::vector<Value>&, const Value* a)    { return _stub("cond-expand", a); }
static Value _form_define_library(Context*, Environment*, std::vector<Value>&, const Value* a) { return _stub("define-library", a); }
static Value _form_import(Context*, Environment*, std::vector<Value>&, const Value* a)         { return _stub("import", a); }
static Value _form_export(Context*, Environment*, std::vector<Value>&, const Value* a)         { return _stub("export", a); }

void register_modules() {
    register_primitive("include", 1, -1, _form_include,
        "(include <filename>...)",
        "Splice the contents of one or more Scheme source files into the\n"
        "enclosing program.  Each filename must be a string literal.  The\n"
        "included forms are parsed and expanded as if typed in place,\n"
        "wrapped in an implicit (begin ...).  R7RS 5.6.1.",
        CATEGORY, SPECIAL);

    register_primitive("include-ci", 1, -1, _form_include_ci,
        "(include-ci <filename>...)",
        "Like include, but symbol names in the included source are\n"
        "case-folded to lowercase (R7RS 5.6.1).  Useful for consuming\n"
        "traditional Lisp source that relies on case-insensitive reads.",
        CATEGORY, SPECIAL);

    register_primitive("cond-expand", 1, -1, _form_cond_expand,
        "(cond-expand <clause>...)",
        "Expand-time conditional.  Each clause has the form\n"
        "   (<feature-requirement> <body>...)\n"
        "or (else <body>...).  The first clause whose <feature-requirement>\n"
        "is satisfied by the current implementation is selected; its body\n"
        "is spliced in place of the cond-expand form.  Feature requirements\n"
        "are feature identifiers (r7rs, exact-closed, pyscheme), (and ...),\n"
        "(or ...), (not ...), or (library <name>).  R7RS 5.6.2.",
        CATEGORY, SPECIAL);

    register_primitive("define-library", 2, -1, _form_define_library,
        "(define-library <name> <decl>...)",
        "Declare a library named <name> (a list of symbols/integers,\n"
        "e.g. (scheme base) or (my utilities 1)).  Each <decl> is one\n"
        "of:\n"
        "  (import <import-set>...)   - bindings visible inside the library\n"
        "  (export <spec>...)         - names to expose; each spec is a\n"
        "                               symbol or (rename <int> <ext>)\n"
        "  (begin <form>...)          - definitions populating the lib env\n"
        "The library is registered in the global library registry and\n"
        "becomes available to (import ...).  R7RS 5.6.",
        CATEGORY, SPECIAL);

    register_primitive("import", 1, -1, _form_import,
        "(import <import-set>...)",
        "Import bindings from one or more libraries into the current\n"
        "environment.  Each <import-set> is either a library name or one\n"
        "of (only ... n...), (except ... n...), (rename ... (o n)...),\n"
        "(prefix ... p).  At top level, bindings are added to the global\n"
        "env; inside (define-library ...) they populate that library's\n"
        "isolated env.  R7RS 5.6.",
        CATEGORY, SPECIAL);

    register_primitive("export", 1, -1, _form_export,
        "(export <spec>...)",
        "Valid only inside (define-library ...) as a declaration.  Each\n"
        "<spec> is a symbol or (rename <internal> <external>).  R7RS 5.6.",
        CATEGORY, SPECIAL);
}
