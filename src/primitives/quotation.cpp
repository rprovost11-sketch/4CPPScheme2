// primitives/quotation.cpp -- quotation special form stubs.
// Direct port of pyscheme/primitives/quotation.py.
#include "quotation.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"

static const char* CATEGORY = "quotation";
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

static Value _form_quote(Context*, Environment*, std::vector<Value>&, const Value* a)            { return _stub("quote", a); }
static Value _form_quasiquote(Context*, Environment*, std::vector<Value>&, const Value* a)       { return _stub("quasiquote", a); }
static Value _form_unquote(Context*, Environment*, std::vector<Value>&, const Value* a)          { return _stub("unquote", a); }
static Value _form_unquote_splicing(Context*, Environment*, std::vector<Value>&, const Value* a) { return _stub("unquote-splicing", a); }

void register_quotation() {
    register_primitive("quote", 1, 1, _form_quote,
        "(quote <datum>)",
        "Return the datum unevaluated.  'x is shorthand for (quote x).",
        CATEGORY, SPECIAL);

    register_primitive("quasiquote", 1, 1, _form_quasiquote,
        "(quasiquote <template>)",
        "Return a value shaped like <template>, but with any (unquote e)\n"
        "holes replaced by the value of e, and any (unquote-splicing e)\n"
        "holes expanded by splicing the elements of e's list into the\n"
        "surrounding list.  Reader syntax: `x == (quasiquote x),\n"
        ",e == (unquote e), ,@e == (unquote-splicing e).  R7RS 4.2.8.",
        CATEGORY, SPECIAL);

    register_primitive("unquote", 1, 1, _form_unquote,
        "(unquote <expr>)",
        "Unquote marker, valid only inside a quasiquote template.",
        CATEGORY, SPECIAL);

    register_primitive("unquote-splicing", 1, 1, _form_unquote_splicing,
        "(unquote-splicing <expr>)",
        "Splicing-unquote marker, valid only inside a quasiquote template.",
        CATEGORY, SPECIAL);
}
