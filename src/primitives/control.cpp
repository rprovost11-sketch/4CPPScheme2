// primitives/control.cpp -- control flow primitives.
// Direct port of pyscheme/primitives/control.py.
#include "control.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"

static const char* CATEGORY = "control";
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

static Value _form_if(Context*, Environment*, std::vector<Value>&, const Value* a)           { return _stub("if", a); }
static Value _form_when(Context*, Environment*, std::vector<Value>&, const Value* a)         { return _stub("when", a); }
static Value _form_unless(Context*, Environment*, std::vector<Value>&, const Value* a)       { return _stub("unless", a); }
static Value _form_cond(Context*, Environment*, std::vector<Value>&, const Value* a)         { return _stub("cond", a); }
static Value _form_case(Context*, Environment*, std::vector<Value>&, const Value* a)         { return _stub("case", a); }
static Value _form_do(Context*, Environment*, std::vector<Value>&, const Value* a)           { return _stub("do", a); }
static Value _form_begin(Context*, Environment*, std::vector<Value>&, const Value* a)        { return _stub("begin", a); }
static Value _form_guard(Context*, Environment*, std::vector<Value>&, const Value* a)        { return _stub("guard", a); }
static Value _form_parameterize(Context*, Environment*, std::vector<Value>&, const Value* a) { return _stub("parameterize", a); }

static Value _prim_error_unreached(Context*, Environment*, std::vector<Value>&, const Value* a) {
    throw SchemeTypeError(
        "error: cannot be called through a re-entering path in this implementation", _src(a));
}

static Value _prim_raise_unreached(Context*, Environment*, std::vector<Value>&, const Value* a) {
    throw SchemeTypeError(
        "raise: cannot be called through a re-entering path in this implementation", _src(a));
}

static Value _prim_raise_continuable_unreached(Context*, Environment*, std::vector<Value>&, const Value* a) {
    throw SchemeTypeError(
        "raise-continuable: cannot be called through a re-entering path in this implementation", _src(a));
}

static Value _prim_with_exception_handler_unreached(Context*, Environment*, std::vector<Value>&, const Value* a) {
    throw SchemeTypeError(
        "with-exception-handler: cannot be called through a re-entering path in this implementation", _src(a));
}

static Value _prim_values(Context*, Environment*, std::vector<Value>& args, const Value* a) {
    if (args.size() == 1) return args[0];
    return make_multi_values(std::vector<Value>(args.begin(), args.end()), _src(a));
}

static Value _prim_call_with_values_unreached(Context*, Environment*, std::vector<Value>&, const Value* a) {
    throw SchemeTypeError(
        "call-with-values: cannot be called through a re-entering path in this implementation", _src(a));
}

static Value _prim_call_cc_unreached(Context*, Environment*, std::vector<Value>&, const Value* a) {
    throw SchemeTypeError(
        "call/cc: cannot be applied through a re-entering primitive "
        "(apply / call-with-values / force) in this implementation", _src(a));
}

static Value _prim_dynamic_wind_unreached(Context*, Environment*, std::vector<Value>&, const Value* a) {
    throw SchemeTypeError(
        "dynamic-wind: cannot be applied through a re-entering primitive "
        "(apply / call-with-values / force) in this implementation", _src(a));
}

static Value _prim_guard_eval_unreached(Context*, Environment*, std::vector<Value>&, const Value* a) {
    throw SchemeTypeError(
        "%guard-eval: cannot be called through a re-entering path in this implementation", _src(a));
}

void register_control() {
    register_primitive("if", 2, 3, _form_if,
        "(if <test> <then> [<else>])",
        "Evaluate <test>.  If truthy, evaluate and return <then>; otherwise\n"
        "evaluate and return <else> if present, or an unspecified value if not.\n"
        "In Scheme only #f is false; every other value (including 0 and '()) is\n"
        "truthy.",
        CATEGORY, SPECIAL);

    register_primitive("when", 2, -1, _form_when,
        "(when <test> <body>...)",
        "If <test> is truthy, evaluate <body> in sequence and return the\n"
        "last value.  Otherwise, return an unspecified value.",
        CATEGORY, SPECIAL);

    register_primitive("unless", 2, -1, _form_unless,
        "(unless <test> <body>...)",
        "If <test> is false, evaluate <body> in sequence and return the\n"
        "last value.  Otherwise, return an unspecified value.",
        CATEGORY, SPECIAL);

    register_primitive("cond", 1, -1, _form_cond,
        "(cond <clause>...)",
        "Evaluate clauses in order.  Each clause has one of the forms:\n"
        "(<test>)                -> returns the test value if truthy\n"
        "(<test> <expr>...)      -> evaluates the body if the test is truthy\n"
        "(<test> => <proc>)      -> if truthy, applies <proc> to the test value\n"
        "(else <expr>...)        -> unconditional; must be the last clause\n"
        "If no clause matches, returns an unspecified value.",
        CATEGORY, SPECIAL);

    register_primitive("case", 2, -1, _form_case,
        "(case <key> <clause>...)",
        "Evaluate <key>, then match its value against each clause's datum\n"
        "list using eqv?.  Each clause has one of the forms:\n"
        "((<datum>...) <expr>...)  -> run body if key matches any datum\n"
        "(else <expr>...)          -> unconditional; must be the last clause\n"
        "Datums are implicitly quoted and are not evaluated.  If no clause\n"
        "matches, returns an unspecified value.",
        CATEGORY, SPECIAL);

    register_primitive("do", 2, -1, _form_do,
        "(do ((<var> <init> [<step>])...) (<test> <result>...) <command>...)",
        "Iterative construct.  Bind each <var> to its <init>, then repeatedly:\n"
        "  evaluate <test>; if true, evaluate <result> expressions in order and\n"
        "  return the last value (or an unspecified value if <result> is empty);\n"
        "  otherwise evaluate <command>s for effect, evaluate each <step> in\n"
        "  the current env, rebind each <var> to its step's value, then loop.\n"
        "A binding of the form (<var> <init>) keeps <var> at its current value\n"
        "each iteration (no step).  Implemented as a desugar to named let.",
        CATEGORY, SPECIAL);

    register_primitive("begin", 1, -1, _form_begin,
        "(begin <expr>...)",
        "Evaluate expressions in sequence; return the value of the last one.",
        CATEGORY, SPECIAL);

    register_primitive("guard", 2, -1, _form_guard,
        "(guard (<var> <clause>...) <body>...)",
        "Install an exception handler for <body>.  If raise or error fires\n"
        "during <body>, the handler binds <var> to the raised value and\n"
        "evaluates the <clause>s in cond-like fashion.  Clauses follow the\n"
        "same grammar as cond.  If no clause matches and no else is given,\n"
        "the raised value is re-raised in the outer scope.  R7RS 4.2.7.",
        CATEGORY, SPECIAL);

    register_primitive("error", 1, -1, _prim_error_unreached,
        "",
        "Raise a user error.  The first argument is a string message;\n"
        "any trailing arguments are appended to the message as irritants\n"
        "separated by spaces.  Does not return.",
        CATEGORY);

    register_primitive("raise", 1, 1, _prim_raise_unreached,
        "",
        "Raise a non-continuable exception carrying the given value.\n"
        "If a with-exception-handler (or guard) handler catches it, the\n"
        "handler's return value is discarded and a secondary error fires\n"
        "because there is no valid continuation to return to.  R7RS 6.11.",
        CATEGORY);

    register_primitive("raise-continuable", 1, 1, _prim_raise_continuable_unreached,
        "",
        "Raise a continuable exception carrying the given value.  When\n"
        "caught by with-exception-handler, the handler's return value\n"
        "becomes the return value of (raise-continuable ...).  R7RS 6.11.",
        CATEGORY);

    register_primitive("with-exception-handler", 2, 2,
        _prim_with_exception_handler_unreached,
        "(with-exception-handler <handler> <thunk>)",
        "Install <handler> for the dynamic extent of (<thunk>).  When a\n"
        "raise or raise-continuable fires inside <thunk>, call <handler>\n"
        "with the raised value.  <handler> is a 1-arg procedure; <thunk>\n"
        "is a 0-arg procedure.  R7RS 6.11.",
        CATEGORY);

    register_primitive("parameterize", 2, -1, _form_parameterize,
        "(parameterize ((<param> <val>)...) <body>...)",
        "Dynamically bind parameter objects for the extent of <body>.\n"
        "Each <param> must evaluate to a parameter (made by make-parameter).\n"
        "Each <val> is bound as the parameter's dynamic value (converted\n"
        "if the parameter has a converter).  When <body> returns, the\n"
        "original values are restored - even if <body> raises.  R7RS 4.2.6.",
        CATEGORY, SPECIAL);

    register_primitive("values", 0, -1, _prim_values,
        "(values <obj>...)",
        "Return the arguments as multiple values.  With zero arguments,\n"
        "returns an empty multi-values container.  With one argument,\n"
        "returns that value unchanged (no wrapper).  With two or more,\n"
        "returns a multi-values container that only call-with-values\n"
        "(and a few related forms) can consume; delivering multi-values\n"
        "to a single-value context is an error.",
        CATEGORY);

    register_primitive("call-with-values", 2, 2, _prim_call_with_values_unreached,
        "(call-with-values <producer> <consumer>)",
        "Call <producer> with no arguments.  Pass its return value(s)\n"
        "to <consumer>: if <producer> returned a multi-values container,\n"
        "each value becomes a separate argument to <consumer>; otherwise\n"
        "<consumer> is called with the single value.  Returns <consumer>'s\n"
        "result.",
        CATEGORY);

    register_primitive("call-with-current-continuation", 1, 1, _prim_call_cc_unreached,
        "(call-with-current-continuation <proc>)",
        "Capture the current continuation as a first-class procedure and\n"
        "apply <proc> to it.  Invoking the continuation with zero or more\n"
        "values abandons the current context and returns to call/cc's\n"
        "caller with those values.  R7RS 6.10.",
        CATEGORY);

    register_primitive("call/cc", 1, 1, _prim_call_cc_unreached,
        "(call/cc <proc>)",
        "Alias for call-with-current-continuation.",
        CATEGORY);

    register_primitive("%guard-eval", 2, 2, _prim_guard_eval_unreached,
        "",
        "Internal: guard body evaluator (FRAME_GUARD, no call/cc chain).",
        CATEGORY);

    register_primitive("dynamic-wind", 3, 3, _prim_dynamic_wind_unreached,
        "(dynamic-wind <before> <thunk> <after>)",
        "Call <before> for effect, then <thunk> for value, then <after>\n"
        "for effect.  The after thunk runs whether <thunk> returns normally\n"
        "or control leaves via a continuation invocation or an exception.\n"
        "If the dynamic extent is later re-entered via a continuation,\n"
        "<before> runs again.  R7RS 6.10.",
        CATEGORY);
}
