// primitives/logical.cpp -- logical primitives.
// Direct port of pyscheme/primitives/logical.py.
#include "logical.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"

static const char* CATEGORY = "logical";
static const char* SPECIAL = "special";

static Value _prim_not(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(!is_truthy(args[0]));
   }

static Value _form_and(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   throw SchemeTypeError("'and' is a special form, not a procedure.",
                         a ? src_of(*a) : nullptr);
   }

static Value _form_or(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   throw SchemeTypeError("'or' is a special form, not a procedure.",
                         a ? src_of(*a) : nullptr);
   }

void register_logical()
   {
   register_primitive("not", 1, 1, _prim_not,
                      "",
                      "Return #t if a is #f, and #f otherwise.  In Scheme only #f is\n"
                      "considered false; every other value (including 0 and the empty list)\n"
                      "is truthy.",
                      CATEGORY);

   register_primitive("and", 0, -1, _form_and,
                      "(and <expr>...)",
                      "Evaluate expressions left to right.  Short-circuit and return #f\n"
                      "on the first false expression; otherwise return the last value.  With no\n"
                      "arguments, returns #t.",
                      CATEGORY, SPECIAL);

   register_primitive("or", 0, -1, _form_or,
                      "(or <expr>...)",
                      "Evaluate expressions left to right.  Short-circuit and return\n"
                      "the first truthy value; otherwise return #f.  With no arguments, returns #f.",
                      CATEGORY, SPECIAL);
   }
