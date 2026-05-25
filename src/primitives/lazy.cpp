// primitives/lazy.cpp -- lazy evaluation primitives.
// Direct port of pyscheme/primitives/lazy.py.
#include "lazy.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"

static const char* CATEGORY = "lazy";
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

static Value _form_delay(Context*, Environment*, std::vector<Value>&, const Value* a)       { return _stub("delay", a); }
static Value _form_delay_force(Context*, Environment*, std::vector<Value>&, const Value* a) { return _stub("delay-force", a); }

static Value _prim_force_unreached(Context*, Environment*, std::vector<Value>&, const Value* a) {
    throw SchemeTypeError(
        "force: cannot be called through a re-entering path in this implementation", _src(a));
}

static Value _prim_make_promise(Context*, Environment*, std::vector<Value>& args, const Value*) {
    return make_promise_done(args[0]);
}

static Value _prim_promise_p(Context*, Environment*, std::vector<Value>& args, const Value*) {
    return make_boolean(is_promise(args[0]));
}

void register_lazy() {
    register_primitive("delay", 1, 1, _form_delay,
        "(delay <expr>)",
        "Return a promise whose forced value is the value of <expr>.\n"
        "<expr> is not evaluated until the promise is forced; its value is\n"
        "then cached so subsequent forces are O(1).  R7RS 4.2.5.  Use force\n"
        "to retrieve the value.",
        CATEGORY, SPECIAL);

    register_primitive("delay-force", 1, 1, _form_delay_force,
        "(delay-force <expr>)",
        "Like delay, but in a form that permits stack-safe iterative\n"
        "forcing: if <expr> itself evaluates to a promise, force collapses\n"
        "the outer promise into the inner one rather than nesting.  Use\n"
        "this when building long lazy chains that force tail-recursively.\n"
        "R7RS 4.2.5.",
        CATEGORY, SPECIAL);

    register_primitive("force", 1, 1, _prim_force_unreached,
        "",
        "Force a promise, returning its value.  The promise's thunk runs\n"
        "at most once; subsequent forces return the cached value.  If the\n"
        "thunk yields another promise, force follows the chain iteratively,\n"
        "so (delay-force ...) promise chains run in constant stack.  A\n"
        "non-promise argument is returned unchanged (R7RS-small 6.10 leaves\n"
        "this implementation-defined; we follow SRFI 155).",
        CATEGORY);

    register_primitive("make-promise", 1, 1, _prim_make_promise,
        "",
        "Return a promise whose forced value is obj.  Unlike delay,\n"
        "make-promise is a procedure, not a special form: its argument\n"
        "is evaluated eagerly and the resulting promise is already forced.",
        CATEGORY);

    register_primitive("promise?", 1, 1, _prim_promise_p,
        "",
        "Return #t if a is a promise (created by delay, delay-force, or make-promise).",
        CATEGORY);
}
