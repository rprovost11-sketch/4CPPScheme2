// primitives/predicates.cpp -- type and numeric predicate primitives.
// Direct port of pyscheme/primitives/predicates.py.
#include "predicates.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"
#include "../rational.h"
#include "../mini-gmp/mini-gmp.h"
#include <cmath>

static const char* CATEGORY = "predicates";

static SourceInfo* _src(const Value* a)
   {
   return a ? src_of(*a) : nullptr;
   }

static bool _is_number(const Value& v)
   {
   return is_integer(v) || is_bignum(v) || is_real(v) || is_rational(v) || is_complex(v) || is_exact_complex(v);
   }

static Value _prim_number_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(_is_number(args[0]));
   }

static Value _prim_complex_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(_is_number(args[0]));
   }

static Value _prim_real_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   const Value& v = args[0];
   if (is_integer(v) || is_bignum(v) || is_real(v) || is_rational(v))
      return make_boolean(true);
   if (is_complex(v))
      return make_boolean(as_complex_imag(v) == 0.0);
   if (is_exact_complex(v))
      {
      Value im = as_exact_complex_imag(v);
      return make_boolean(is_integer(im) && as_integer(im) == 0);
      }
   return make_boolean(false);
   }

static Value _prim_rational_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   const Value& v = args[0];
   if (is_integer(v) || is_bignum(v) || is_rational(v))
      return make_boolean(true);
   if (is_real(v))
      return make_boolean(std::isfinite(as_real(v)));
   if (is_complex(v))
      return make_boolean(as_complex_imag(v) == 0.0 && std::isfinite(as_complex_real(v)));
   if (is_exact_complex(v))
      {
      Value im = as_exact_complex_imag(v);
      return make_boolean(is_integer(im) && as_integer(im) == 0);
      }
   return make_boolean(false);
   }

static Value _prim_integer_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   const Value& v = args[0];
   if (is_integer(v) || is_bignum(v))
      return make_boolean(true);
   if (is_real(v))
      {
      double r = as_real(v);
      return make_boolean(std::isfinite(r) && std::floor(r) == r);
      }
   if (is_complex(v))
      {
      double re = as_complex_real(v), im = as_complex_imag(v);
      return make_boolean(im == 0.0 && std::floor(re) == re);
      }
   if (is_exact_complex(v))
      {
      Value im = as_exact_complex_imag(v);
      if (!is_integer(im) || as_integer(im) != 0)
         return make_boolean(false);
      return make_boolean(is_integer(as_exact_complex_real(v)));
      }
   return make_boolean(false);
   }

// Extract real number for positive?/negative?/zero? -- error on non-real.
static double _real_or_error(const Value& v, const char* name, const Value* app)
   {
   if (is_integer(v))
      return static_cast<double>(as_integer(v));
   if (is_bignum(v))
      return mpz_get_d(as_bignum(v));
   if (is_rational(v))
      return static_cast<double>(Rat{as_rational_num(v), as_rational_den(v)});
   if (is_real(v))
      return as_real(v);
   if (is_complex(v) && as_complex_imag(v) == 0.0)
      return as_complex_real(v);
   if (is_exact_complex(v))
      {
      Value im = as_exact_complex_imag(v);
      if (is_integer(im) && as_integer(im) == 0)
         {
         Value re = as_exact_complex_real(v);
         if (is_integer(re))
            return static_cast<double>(as_integer(re));
         if (is_rational(re))
            return static_cast<double>(Rat{as_rational_num(re), as_rational_den(re)});
         }
      }
   throw SchemeTypeError(std::string(name) + ": argument is not a real number", _src(app));
   }

static Value _prim_zero_p(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (is_complex(v))
      return make_boolean(as_complex_real(v) == 0.0 && as_complex_imag(v) == 0.0);
   if (is_exact_complex(v))
      {
      Value re = as_exact_complex_real(v);
      Value im = as_exact_complex_imag(v);
      return make_boolean(is_integer(re) && as_integer(re) == 0 &&
                          is_integer(im) && as_integer(im) == 0);
      }
   return make_boolean(_real_or_error(v, "zero?", app) == 0.0);
   }

static Value _prim_positive_p(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   return make_boolean(_real_or_error(args[0], "positive?", app) > 0.0);
   }

static Value _prim_negative_p(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   return make_boolean(_real_or_error(args[0], "negative?", app) < 0.0);
   }

static int64_t _int_or_error(const Value& v, const char* name, const Value* app)
   {
   if (is_integer(v))
      return as_integer(v);
   if (is_real(v))
      {
      double r = as_real(v);
      if (std::isfinite(r) && std::floor(r) == r)
         return static_cast<int64_t>(r);
      }
   throw SchemeTypeError(
       std::string(name) + ": expected integer, got non-integer value", _src(app));
   }

static Value _prim_even_p(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (is_bignum(v))
      return make_boolean(mpz_divisible_ui_p(as_bignum(v), 2) != 0);
   return make_boolean(_int_or_error(v, "even?", app) % 2 == 0);
   }

static Value _prim_odd_p(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (is_bignum(v))
      return make_boolean(mpz_divisible_ui_p(as_bignum(v), 2) == 0);
   int64_t n = _int_or_error(v, "odd?", app);
   return make_boolean(n % 2 != 0);
   }

static Value _prim_finite_p(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (is_integer(v) || is_bignum(v) || is_rational(v) || is_exact_complex(v))
      return make_boolean(true);
   if (is_real(v))
      return make_boolean(std::isfinite(as_real(v)));
   if (is_complex(v))
      return make_boolean(std::isfinite(as_complex_real(v)) && std::isfinite(as_complex_imag(v)));
   throw SchemeTypeError("finite?: argument must be a number", _src(app));
   }

static Value _prim_infinite_p(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (is_integer(v) || is_bignum(v) || is_rational(v) || is_exact_complex(v))
      return make_boolean(false);
   if (is_real(v))
      return make_boolean(std::isinf(as_real(v)));
   if (is_complex(v))
      return make_boolean(std::isinf(as_complex_real(v)) || std::isinf(as_complex_imag(v)));
   throw SchemeTypeError("infinite?: argument must be a number", _src(app));
   }

static Value _prim_nan_p(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   const Value& v = args[0];
   if (is_integer(v) || is_bignum(v) || is_rational(v) || is_exact_complex(v))
      return make_boolean(false);
   if (is_real(v))
      return make_boolean(std::isnan(as_real(v)));
   if (is_complex(v))
      return make_boolean(std::isnan(as_complex_real(v)) || std::isnan(as_complex_imag(v)));
   throw SchemeTypeError("nan?: argument must be a number", _src(app));
   }

static Value _prim_boolean_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_boolean(args[0]));
   }

static Value _prim_procedure_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_procedure(args[0]));
   }

static Value _prim_parameter_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_parameter(args[0]));
   }

static Value _prim_error_object_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_error_object(args[0]));
   }

static Value _prim_symbol_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_symbol(args[0]));
   }

static Value _prim_string_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_string(args[0]));
   }

static Value _prim_immutable_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_immutable(args[0]));
   }

void register_predicates()
   {
   register_primitive("number?", 1, 1, _prim_number_p, "", "Return #t if a is any kind of number.", CATEGORY);
   register_primitive("complex?", 1, 1, _prim_complex_p, "", "Return #t if a is a number.  In the R7RS tower, every number is complex.", CATEGORY);
   register_primitive("real?", 1, 1, _prim_real_p, "", "Return #t if a is a real number (integer, rational, or real).", CATEGORY);
   register_primitive("rational?", 1, 1, _prim_rational_p, "", "Return #t if a is a rational number (integer or rational).", CATEGORY);
   register_primitive("integer?", 1, 1, _prim_integer_p, "",
                      "Return #t if a is an integer.  A REAL with an integer value\n"
                      "(like 3.0) is also considered an integer.",
                      CATEGORY);
   register_primitive("zero?", 1, 1, _prim_zero_p, "", "Return #t if a is numerically equal to zero.", CATEGORY);
   register_primitive("positive?", 1, 1, _prim_positive_p, "", "Return #t if a is strictly greater than zero.", CATEGORY);
   register_primitive("negative?", 1, 1, _prim_negative_p, "", "Return #t if a is strictly less than zero.", CATEGORY);
   register_primitive("even?", 1, 1, _prim_even_p, "", "Return #t if a is an even integer.", CATEGORY);
   register_primitive("odd?", 1, 1, _prim_odd_p, "", "Return #t if a is an odd integer.", CATEGORY);
   register_primitive("boolean?", 1, 1, _prim_boolean_p, "", "Return #t if a is #t or #f.", CATEGORY);
   register_primitive("procedure?", 1, 1, _prim_procedure_p, "", "Return #t if a is callable (a primitive or user-defined closure).", CATEGORY);
   register_primitive("symbol?", 1, 1, _prim_symbol_p, "", "Return #t if a is a symbol.", CATEGORY);
   register_primitive("string?", 1, 1, _prim_string_p, "", "Return #t if a is a string.", CATEGORY);
   register_primitive("parameter?", 1, 1, _prim_parameter_p, "", "Return #t if a is a parameter object (created by make-parameter).", CATEGORY);
   register_primitive("error-object?", 1, 1, _prim_error_object_p, "", "Return #t if a is an error object (produced by the error primitive).", CATEGORY);
   register_primitive("finite?", 1, 1, _prim_finite_p, "", "Return #t if z is a finite number (not +inf.0, -inf.0, or +nan.0).", CATEGORY);
   register_primitive("infinite?", 1, 1, _prim_infinite_p, "", "Return #t if z is a real infinity (+inf.0 or -inf.0).", CATEGORY);
   register_primitive("nan?", 1, 1, _prim_nan_p, "", "Return #t if z is +nan.0.", CATEGORY);
   register_primitive("immutable?", 1, 1, _prim_immutable_p, "", "Return #t if obj is a mutable-type object (pair, string, vector, bytevector) that has been marked immutable.", CATEGORY);
   }
