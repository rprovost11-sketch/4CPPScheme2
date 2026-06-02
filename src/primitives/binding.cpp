// primitives/binding.cpp -- binding special form stubs.
// Direct port of pyscheme/primitives/binding.py.
#include "binding.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"

static const char* CATEGORY = "binding";
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

static Value _form_lambda(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   return _stub("lambda", a);
   }
static Value _form_case_lambda(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   return _stub("case-lambda", a);
   }
static Value _form_define(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   return _stub("define", a);
   }
static Value _form_set(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   return _stub("set!", a);
   }
static Value _form_let(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   return _stub("let", a);
   }
static Value _form_let_star(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   return _stub("let*", a);
   }
static Value _form_letrec(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   return _stub("letrec", a);
   }
static Value _form_letrec_star(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   return _stub("letrec*", a);
   }
static Value _form_let_values(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   return _stub("let-values", a);
   }
static Value _form_let_star_values(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   return _stub("let*-values", a);
   }
static Value _form_define_values(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   return _stub("define-values", a);
   }
static Value _form_define_record_type(Context*, Environment*, std::vector<Value>&, const Value* a)
   {
   return _stub("define-record-type", a);
   }

void register_binding()
   {
   register_primitive("lambda", 2, -1, _form_lambda,
                      "(lambda <formals> <body>...)",
                      "Create a procedure.  <formals> is (v1 v2 ...) for fixed arity,\n"
                      "<var> alone for pure variadic (all arguments collected into <var> as a\n"
                      "list), or (v1 ... . <rest>) for a fixed prefix followed by the remaining\n"
                      "arguments collected into <rest>.  The <body> may begin with a string\n"
                      "literal, which is taken as the procedure's documentation.",
                      CATEGORY, SPECIAL);

   register_primitive("case-lambda", 1, -1, _form_case_lambda,
                      "(case-lambda (<formals> <body>...)...)",
                      "Create an arity-dispatched procedure.  Each clause is shaped\n"
                      "like a lambda: (<formals> <body>...).  When the procedure is\n"
                      "called, the first clause whose <formals> accept the argument\n"
                      "count is selected and its body is run.  <formals> follows the\n"
                      "same shape rules as lambda (fixed list, symbol, or fixed+rest).\n"
                      "R7RS 4.2.9.",
                      CATEGORY, SPECIAL);

   register_primitive("define", 2, 2, _form_define,
                      "(define <name> <value>)",
                      "Bind <name> to the value of <value> in the current environment.\n"
                      "The shorthand (define (<name> <formals>...) <body>...) is equivalent to\n"
                      "(define <name> (lambda (<formals>...) <body>...)).",
                      CATEGORY, SPECIAL);

   register_primitive("set!", 2, 2, _form_set,
                      "(set! <name> <value>)",
                      "Update the existing binding for <name> in the nearest enclosing\n"
                      "scope to the value of <value>.  If <name> has no binding, an error is\n"
                      "raised (unlike Common Lisp's setq which would create a top-level binding).",
                      CATEGORY, SPECIAL);

   register_primitive("let", 2, -1, _form_let,
                      "(let [<name>] ((<var> <init>)...) <body>...)",
                      "Evaluate every <init> in the enclosing env, then bind each <var>\n"
                      "to its corresponding value and evaluate <body>.  With an optional <name>\n"
                      "before the binding list, creates a named let: a local recursive procedure\n"
                      "<name> bound to (lambda (<var>...) <body>...), immediately applied to the\n"
                      "inits.  Useful for iteration.",
                      CATEGORY, SPECIAL);

   register_primitive("let*", 2, -1, _form_let_star,
                      "(let* ((<var> <init>)...) <body>...)",
                      "Like let, but each <init> is evaluated in an environment where\n"
                      "the earlier bindings are in scope.  Equivalent to nested single-binding\n"
                      "lets.",
                      CATEGORY, SPECIAL);

   register_primitive("letrec", 2, -1, _form_letrec,
                      "(letrec ((<var> <init>)...) <body>...)",
                      "Like let, but each <init> is evaluated in an environment where\n"
                      "all the <var>s are already in scope (initially bound to unspecified\n"
                      "values).  Used for mutual recursion.  R7RS does not specify the order\n"
                      "in which inits evaluate; this implementation is sequential.",
                      CATEGORY, SPECIAL);

   register_primitive("letrec*", 2, -1, _form_letrec_star,
                      "(letrec* ((<var> <init>)...) <body>...)",
                      "Like letrec, but evaluates the inits strictly left to right and\n"
                      "guarantees each prior binding is set before the next init runs.",
                      CATEGORY, SPECIAL);

   register_primitive("let-values", 2, -1, _form_let_values,
                      "(let-values ((<formals> <init>)...) <body>...)",
                      "Parallel multi-value bindings.  Each <init> is evaluated in the\n"
                      "outer environment; its return values are bound to the variables\n"
                      "in the matching <formals>.  All bindings become visible together\n"
                      "when <body> runs.  <formals> follows lambda's shape (proper list,\n"
                      "dotted tail, or a single identifier).  R7RS 4.2.2.",
                      CATEGORY, SPECIAL);

   register_primitive("let*-values", 2, -1, _form_let_star_values,
                      "(let*-values ((<formals> <init>)...) <body>...)",
                      "Sequential multi-value bindings.  Like let-values, but each\n"
                      "<init> sees the variables bound by earlier clauses.  R7RS 4.2.2.",
                      CATEGORY, SPECIAL);

   register_primitive("define-values", 2, 2, _form_define_values,
                      "(define-values <formals> <expr>)",
                      "Bind each identifier in <formals> to the corresponding value\n"
                      "produced by <expr>.  <formals> follows lambda's shape: a proper\n"
                      "list, a dotted tail, or a single identifier that receives all\n"
                      "values as a list.  Useful for destructuring a multi-value\n"
                      "producer at top level.  R7RS 5.3.3.",
                      CATEGORY, SPECIAL);

   register_primitive("define-record-type", 4, -1, _form_define_record_type,
                      "(define-record-type <name> (<ctor> <ctor-field>...) <predicate> <field-spec>...)",
                      "Define a record type.  <name> is the type's symbol.  The\n"
                      "constructor clause (<ctor> <ctor-field>...) names the constructor\n"
                      "procedure and lists the fields it initializes (other fields get\n"
                      "#<void>).  <predicate> names the type predicate.  Each <field-spec>\n"
                      "is (<field-name> <accessor>) or (<field-name> <accessor> <mutator>);\n"
                      "the order of field specs defines field indices.  Records are a\n"
                      "disjoint type - a record is never eq? to any other Scheme value.\n"
                      "R7RS 5.5.",
                      CATEGORY, SPECIAL);
   }
