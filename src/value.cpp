#include "value.h"
#include "environment.h"
#include "port.h"
#include "gc.h"
#include <sstream>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <ostream>


// ── Type predicates ──────────────────────────────────────────────────────────

bool is_nil(Value val)         { return std::holds_alternative<std::monostate>(val.repr); }
bool is_bool(Value val)        { return std::holds_alternative<bool>(val.repr); }
bool is_fixnum(Value val)      { return std::holds_alternative<int64_t>(val.repr); }
bool is_flonum(Value val)      { return std::holds_alternative<double>(val.repr); }
bool is_complex(Value val)     { return std::holds_alternative<SchemeComplex*>(val.repr); }
bool is_exact_complex(Value val) { return std::holds_alternative<SchemeExactComplex*>(val.repr); }
bool is_rational(Value val)    { return std::holds_alternative<SchemeRational*>(val.repr); }
bool is_exact_num(Value val)   { return is_fixnum(val) || is_rational(val); }
bool is_number(Value val)
   {
   return is_fixnum(val) || is_flonum(val) || is_complex(val)
       || is_rational(val) || is_exact_complex(val);
   }
bool is_symbol(Value val)      { return std::holds_alternative<SymbolId>(val.repr); }
bool is_cons(Value val)        { return std::holds_alternative<ConsCell*>(val.repr); }
bool is_string(Value val)      { return std::holds_alternative<SchemeString*>(val.repr); }
bool is_closure(Value val)     { return std::holds_alternative<SchemeClosure*>(val.repr); }
bool is_case_closure(Value val){ return std::holds_alternative<SchemeCaseClosure*>(val.repr); }
bool is_macro(Value val)       { return std::holds_alternative<SchemeMacro*>(val.repr); }
bool is_syntax_transformer(Value val)
   { return std::holds_alternative<SchemeSyntaxTransformer*>(val.repr); }
bool is_continuation(Value val){ return std::holds_alternative<SchemeContinuation*>(val.repr); }
bool is_vector(Value val)      { return std::holds_alternative<SchemeVector*>(val.repr); }
bool is_bytevector(Value val)  { return std::holds_alternative<SchemeBytevector*>(val.repr); }
bool is_builtin(Value val)     { return std::holds_alternative<Builtin*>(val.repr); }
bool is_char(Value val)        { return std::holds_alternative<char32_t>(val.repr); }
bool is_eof(Value val)         { return std::holds_alternative<EofTag>(val.repr); }
bool is_port(Value val)        { return std::holds_alternative<SchemePort*>(val.repr); }
bool is_input_port(Value val)
   {
   if (!is_port(val)) return false;
   return std::get<SchemePort*>(val.repr)->is_input;
   }
bool is_output_port(Value val)
   {
   if (!is_port(val)) return false;
   return std::get<SchemePort*>(val.repr)->is_output;
   }
bool is_unspecified(Value val) { return std::holds_alternative<Unspecified>(val.repr); }
bool is_procedure(Value val)
   {
   return is_builtin(val) || is_closure(val) || is_case_closure(val)
       || is_continuation(val) || is_parameter(val);
   }
bool is_promise(Value val)     { return std::holds_alternative<SchemePromise*>(val.repr); }
bool is_parameter(Value val)   { return std::holds_alternative<SchemeParameter*>(val.repr); }
bool is_environment(Value val) { return std::holds_alternative<Environment*>(val.repr); }
bool is_multi_values(Value val){ return std::holds_alternative<SchemeMultiValues*>(val.repr); }
bool is_record(Value val)      { return std::holds_alternative<SchemeRecord*>(val.repr); }
bool is_error_object(Value val){ return std::holds_alternative<SchemeErrorObject*>(val.repr); }

bool is_truthy(Value val)
   {
   if (is_bool(val))
      return std::get<bool>(val.repr);
   return true;
   }


// ── Constructors ─────────────────────────────────────────────────────────────

Value make_nil()                           { return Value{Value::Repr{std::monostate{}}}; }
Value make_bool(bool b)                    { return Value{Value::Repr{b}}; }
Value make_fixnum(int64_t n)               { return Value{Value::Repr{n}}; }
Value make_flonum(double x)                { return Value{Value::Repr{x}}; }
Value make_char(char32_t cp)               { return Value{Value::Repr{cp}}; }
Value make_eof()                           { return Value{Value::Repr{EofTag{}}}; }
Value make_symbol_id(uint32_t sid)         { return Value{Value::Repr{SymbolId{sid}}}; }
Value make_symbol(std::string_view name)   { return make_symbol_id(intern_symbol(name)); }
Value make_cons(ConsCell* p)               { return Value{Value::Repr{p}}; }
Value make_string(SchemeString* p)         { return Value{Value::Repr{p}}; }
Value make_closure(SchemeClosure* p)       { return Value{Value::Repr{p}}; }
Value make_case_closure(SchemeCaseClosure* p) { return Value{Value::Repr{p}}; }
Value make_macro(SchemeMacro* p)           { return Value{Value::Repr{p}}; }
Value make_syntax_transformer(SchemeSyntaxTransformer* p) { return Value{Value::Repr{p}}; }
Value make_continuation(SchemeContinuation* p) { return Value{Value::Repr{p}}; }
Value make_vector(SchemeVector* p)         { return Value{Value::Repr{p}}; }
Value make_bytevector(SchemeBytevector* p) { return Value{Value::Repr{p}}; }
Value make_builtin(Builtin* p)             { return Value{Value::Repr{p}}; }
Value make_unspecified()                   { return Value{Value::Repr{Unspecified{}}}; }
Value make_promise_val(SchemePromise* p)   { return Value{Value::Repr{p}}; }
Value make_parameter_val(SchemeParameter* p) { return Value{Value::Repr{p}}; }
Value make_port(SchemePort* p)             { return Value{Value::Repr{p}}; }
Value make_environment_val(Environment* p) { return Value{Value::Repr{p}}; }
Value make_complex_val(double r, double i) { return Value{Value::Repr{gc_alloc_complex(r, i)}}; }
Value make_rational(SchemeRational* p)     { return Value{Value::Repr{p}}; }
Value make_multi_values_val(SchemeMultiValues* p) { return Value{Value::Repr{p}}; }
Value make_record_val(SchemeRecord* p)     { return Value{Value::Repr{p}}; }
Value make_exact_complex_val(SchemeExactComplex* p) { return Value{Value::Repr{p}}; }
Value make_error_object_val(SchemeErrorObject* p) { return Value{Value::Repr{p}}; }


// ── Accessors ─────────────────────────────────────────────────────────────────

static std::string type_name_of(Value val)
   {
   if (is_nil(val))           return "nil";
   if (is_bool(val))          return "boolean";
   if (is_fixnum(val))        return "fixnum";
   if (is_flonum(val))        return "flonum";
   if (is_char(val))          return "char";
   if (is_eof(val))           return "eof-object";
   if (is_symbol(val))        return "symbol";
   if (is_cons(val))          return "pair";
   if (is_string(val))        return "string";
   if (is_closure(val))       return "procedure";
   if (is_case_closure(val))  return "procedure";
   if (is_macro(val))         return "macro";
   if (is_syntax_transformer(val)) return "syntax-transformer";
   if (is_continuation(val))  return "continuation";
   if (is_vector(val))        return "vector";
   if (is_bytevector(val))    return "bytevector";
   if (is_builtin(val))       return "procedure";
   if (is_unspecified(val))   return "unspecified";
   if (is_parameter(val))     return "parameter";
   if (is_complex(val))       return "complex";
   if (is_exact_complex(val)) return "exact-complex";
   if (is_rational(val))      return "rational";
   if (is_environment(val))   return "environment";
   if (is_port(val))          return "port";
   if (is_multi_values(val))  return "values";
   if (is_record(val))        return "record";
   if (is_error_object(val))  return "error-object";
   return "unknown";
   }

bool as_bool(Value val)
   {
   if (!is_bool(val)) throw SchemeTypeError("boolean", type_name_of(val));
   return std::get<bool>(val.repr);
   }

int64_t as_fixnum(Value val)
   {
   if (!is_fixnum(val)) throw SchemeTypeError("fixnum", type_name_of(val));
   return std::get<int64_t>(val.repr);
   }

double as_flonum(Value val)
   {
   if (!is_flonum(val)) throw SchemeTypeError("flonum", type_name_of(val));
   return std::get<double>(val.repr);
   }

double to_double(Value val)
   {
   if (is_fixnum(val))   return static_cast<double>(std::get<int64_t>(val.repr));
   if (is_flonum(val))   return std::get<double>(val.repr);
   if (is_rational(val))
      {
      auto* r = std::get<SchemeRational*>(val.repr);
      return static_cast<double>(r->num) / static_cast<double>(r->den);
      }
   throw SchemeTypeError("number", type_name_of(val));
   }

char32_t as_char(Value val)
   {
   if (!is_char(val)) throw SchemeTypeError("char", type_name_of(val));
   return std::get<char32_t>(val.repr);
   }

uint32_t as_symbol_id(Value val)
   {
   if (!is_symbol(val)) throw SchemeTypeError("symbol", type_name_of(val));
   return std::get<SymbolId>(val.repr).id;
   }

ConsCell* as_cons(Value val)
   {
   if (!is_cons(val)) throw SchemeTypeError("pair", type_name_of(val));
   return std::get<ConsCell*>(val.repr);
   }

SchemeString* as_string(Value val)
   {
   if (!is_string(val)) throw SchemeTypeError("string", type_name_of(val));
   return std::get<SchemeString*>(val.repr);
   }

SchemeClosure* as_closure(Value val)
   {
   if (!is_closure(val)) throw SchemeTypeError("closure", type_name_of(val));
   return std::get<SchemeClosure*>(val.repr);
   }

SchemeCaseClosure* as_case_closure(Value val)
   {
   if (!is_case_closure(val)) throw SchemeTypeError("case-closure", type_name_of(val));
   return std::get<SchemeCaseClosure*>(val.repr);
   }

SchemeMacro* as_macro(Value val)
   {
   if (!is_macro(val)) throw SchemeTypeError("macro", type_name_of(val));
   return std::get<SchemeMacro*>(val.repr);
   }

SchemeSyntaxTransformer* as_syntax_transformer(Value val)
   {
   if (!is_syntax_transformer(val)) throw SchemeTypeError("syntax-transformer", type_name_of(val));
   return std::get<SchemeSyntaxTransformer*>(val.repr);
   }

SchemeContinuation* as_continuation(Value val)
   {
   if (!is_continuation(val)) throw SchemeTypeError("continuation", type_name_of(val));
   return std::get<SchemeContinuation*>(val.repr);
   }

SchemeVector* as_vector(Value val)
   {
   if (!is_vector(val)) throw SchemeTypeError("vector", type_name_of(val));
   return std::get<SchemeVector*>(val.repr);
   }

SchemeBytevector* as_bytevector(Value val)
   {
   if (!is_bytevector(val)) throw SchemeTypeError("bytevector", type_name_of(val));
   return std::get<SchemeBytevector*>(val.repr);
   }

Builtin* as_builtin(Value val)
   {
   if (!is_builtin(val)) throw SchemeTypeError("builtin", type_name_of(val));
   return std::get<Builtin*>(val.repr);
   }

SchemePromise* as_promise(Value val)
   {
   if (!is_promise(val)) throw SchemeTypeError("promise", type_name_of(val));
   return std::get<SchemePromise*>(val.repr);
   }

SchemeParameter* as_parameter(Value val)
   {
   if (!is_parameter(val)) throw SchemeTypeError("parameter", type_name_of(val));
   return std::get<SchemeParameter*>(val.repr);
   }

SchemePort* as_port(Value val)
   {
   if (!is_port(val)) throw SchemeTypeError("port", type_name_of(val));
   return std::get<SchemePort*>(val.repr);
   }

Environment* as_environment_val(Value val)
   {
   if (!is_environment(val)) throw SchemeTypeError("environment", type_name_of(val));
   return std::get<Environment*>(val.repr);
   }

SchemeComplex* as_complex(Value val)
   {
   if (!is_complex(val)) throw SchemeTypeError("complex", type_name_of(val));
   return std::get<SchemeComplex*>(val.repr);
   }

SchemeRational* as_rational(Value val)
   {
   if (!is_rational(val)) throw SchemeTypeError("rational", type_name_of(val));
   return std::get<SchemeRational*>(val.repr);
   }

SchemeMultiValues* as_multi_values(Value val)
   {
   if (!is_multi_values(val)) throw SchemeTypeError("values", type_name_of(val));
   return std::get<SchemeMultiValues*>(val.repr);
   }

SchemeRecord* as_record(Value val)
   {
   if (!is_record(val)) throw SchemeTypeError("record", type_name_of(val));
   return std::get<SchemeRecord*>(val.repr);
   }

SchemeExactComplex* as_exact_complex(Value val)
   {
   if (!is_exact_complex(val)) throw SchemeTypeError("exact-complex", type_name_of(val));
   return std::get<SchemeExactComplex*>(val.repr);
   }

SchemeErrorObject* as_error_object(Value val)
   {
   if (!is_error_object(val)) throw SchemeTypeError("error-object", type_name_of(val));
   return std::get<SchemeErrorObject*>(val.repr);
   }


// ── Pair operations ───────────────────────────────────────────────────────────

Value car(Value val) { return as_cons(val)->car; }
Value cdr(Value val) { return as_cons(val)->cdr; }

void set_car(Value cons_val, Value new_car)
   {
   ConsCell* cell = as_cons(cons_val);
   gc_write_barrier(&cell->header, gc_value_header(new_car));
   cell->car = new_car;
   }

void set_cdr(Value cons_val, Value new_cdr)
   {
   ConsCell* cell = as_cons(cons_val);
   gc_write_barrier(&cell->header, gc_value_header(new_cdr));
   cell->cdr = new_cdr;
   }


// ── Equality ──────────────────────────────────────────────────────────────────

bool values_eq(Value lhs, Value rhs)
   {
   if (lhs.repr.index() != rhs.repr.index())
      return false;
   if (is_nil(lhs))          return true;
   if (is_bool(lhs))         return std::get<bool>(lhs.repr)    == std::get<bool>(rhs.repr);
   if (is_fixnum(lhs))       return std::get<int64_t>(lhs.repr) == std::get<int64_t>(rhs.repr);
   if (is_flonum(lhs))       return std::get<double>(lhs.repr)  == std::get<double>(rhs.repr);
   if (is_char(lhs))         return std::get<char32_t>(lhs.repr) == std::get<char32_t>(rhs.repr);
   if (is_eof(lhs))          return true;
   if (is_symbol(lhs))       return std::get<SymbolId>(lhs.repr).id == std::get<SymbolId>(rhs.repr).id;
   if (is_cons(lhs))         return std::get<ConsCell*>(lhs.repr)      == std::get<ConsCell*>(rhs.repr);
   if (is_string(lhs))       return std::get<SchemeString*>(lhs.repr)  == std::get<SchemeString*>(rhs.repr);
   if (is_closure(lhs))      return std::get<SchemeClosure*>(lhs.repr) == std::get<SchemeClosure*>(rhs.repr);
   if (is_case_closure(lhs)) return std::get<SchemeCaseClosure*>(lhs.repr) == std::get<SchemeCaseClosure*>(rhs.repr);
   if (is_macro(lhs))        return std::get<SchemeMacro*>(lhs.repr)   == std::get<SchemeMacro*>(rhs.repr);
   if (is_syntax_transformer(lhs))
      return std::get<SchemeSyntaxTransformer*>(lhs.repr) == std::get<SchemeSyntaxTransformer*>(rhs.repr);
   if (is_continuation(lhs)) return std::get<SchemeContinuation*>(lhs.repr) == std::get<SchemeContinuation*>(rhs.repr);
   if (is_vector(lhs))       return std::get<SchemeVector*>(lhs.repr)  == std::get<SchemeVector*>(rhs.repr);
   if (is_builtin(lhs))      return std::get<Builtin*>(lhs.repr)       == std::get<Builtin*>(rhs.repr);
   if (is_parameter(lhs))    return std::get<SchemeParameter*>(lhs.repr) == std::get<SchemeParameter*>(rhs.repr);
   if (is_environment(lhs))  return std::get<Environment*>(lhs.repr)   == std::get<Environment*>(rhs.repr);
   if (is_complex(lhs))      return std::get<SchemeComplex*>(lhs.repr) == std::get<SchemeComplex*>(rhs.repr);
   if (is_record(lhs))       return std::get<SchemeRecord*>(lhs.repr)  == std::get<SchemeRecord*>(rhs.repr);
   if (is_error_object(lhs)) return std::get<SchemeErrorObject*>(lhs.repr) == std::get<SchemeErrorObject*>(rhs.repr);
   return false;
   }

bool values_eqv(Value lhs, Value rhs)
   {
   if (is_number(lhs) && is_number(rhs))
      {
      bool lhs_exact = is_exact_num(lhs);
      bool rhs_exact = is_exact_num(rhs);
      if (lhs_exact != rhs_exact) return false;

      if (is_fixnum(lhs) && is_fixnum(rhs))
         return std::get<int64_t>(lhs.repr) == std::get<int64_t>(rhs.repr);
      if (lhs_exact)
         return num_eq(lhs, rhs);
      if (is_complex(lhs) || is_complex(rhs))
         {
         double r1 = is_complex(lhs) ? std::get<SchemeComplex*>(lhs.repr)->real : to_double(lhs);
         double i1 = is_complex(lhs) ? std::get<SchemeComplex*>(lhs.repr)->imag : 0.0;
         double r2 = is_complex(rhs) ? std::get<SchemeComplex*>(rhs.repr)->real : to_double(rhs);
         double i2 = is_complex(rhs) ? std::get<SchemeComplex*>(rhs.repr)->imag : 0.0;
         return r1 == r2 && i1 == i2;
         }
      double x = to_double(lhs), y = to_double(rhs);
      if (x == 0.0 && y == 0.0)
         return std::signbit(x) == std::signbit(y);
      return x == y;
      }
   if (is_string(lhs) && is_string(rhs))
      return std::get<SchemeString*>(lhs.repr)->data == std::get<SchemeString*>(rhs.repr)->data;
   return values_eq(lhs, rhs);
   }

bool values_equal(Value lhs, Value rhs)
   {
   if (values_eqv(lhs, rhs)) return true;
   if (is_cons(lhs) && is_cons(rhs))
      return values_equal(car(lhs), car(rhs)) && values_equal(cdr(lhs), cdr(rhs));
   if (is_vector(lhs) && is_vector(rhs))
      {
      auto* lv = as_vector(lhs);
      auto* rv = as_vector(rhs);
      if (lv->elements.size() != rv->elements.size()) return false;
      for (size_t i = 0; i < lv->elements.size(); ++i)
         if (!values_equal(lv->elements[i], rv->elements[i])) return false;
      return true;
      }
   if (is_bytevector(lhs) && is_bytevector(rhs))
      return as_bytevector(lhs)->data == as_bytevector(rhs)->data;
   return false;
   }


// ── Display ───────────────────────────────────────────────────────────────────

static void write_string_escaped(std::ostream& out, const std::string& text)
   {
   out << '"';
   for (char c : text)
      {
      switch (c)
         {
         case '"':  out << "\\\""; break;
         case '\\': out << "\\\\"; break;
         case '\n': out << "\\n";  break;
         case '\r': out << "\\r";  break;
         case '\t': out << "\\t";  break;
         default:   out << c;      break;
         }
      }
   out << '"';
   }

static void format_double(std::ostream& out, double d)
   {
   if (std::isinf(d)) { out << (d > 0 ? "+inf.0" : "-inf.0"); return; }
   if (std::isnan(d)) { out << "+nan.0"; return; }
   std::ostringstream buf;
   buf.precision(std::numeric_limits<double>::max_digits10);
   buf << d;
   std::string s = buf.str();
   if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
       s.find('E') == std::string::npos && s.find('n') == std::string::npos)
      s += ".0";
   out << s;
   }

static void value_to_stream(std::ostream& out, Value val, bool display_mode)
   {
   if (is_nil(val))       { out << "()"; return; }
   if (is_bool(val))      { out << (std::get<bool>(val.repr) ? "#t" : "#f"); return; }
   if (is_fixnum(val))    { out << std::get<int64_t>(val.repr); return; }
   if (is_flonum(val))
      {
      format_double(out, std::get<double>(val.repr));
      return;
      }
   if (is_complex(val))
      {
      auto* c = std::get<SchemeComplex*>(val.repr);
      format_double(out, c->real);
      if      (c->imag ==  1.0) out << "+i";
      else if (c->imag == -1.0) out << "-i";
      else if (std::isnan(c->imag) || c->imag >= 0.0)
         { out << "+"; format_double(out, c->imag); out << "i"; }
      else
         { format_double(out, c->imag); out << "i"; }
      return;
      }
   if (is_exact_complex(val))
      {
      auto* ec = std::get<SchemeExactComplex*>(val.repr);
      value_to_stream(out, ec->real, display_mode);
      out << "+";
      value_to_stream(out, ec->imag, display_mode);
      out << "i";
      return;
      }
   if (is_rational(val))
      {
      auto* r = std::get<SchemeRational*>(val.repr);
      out << r->num << "/" << r->den;
      return;
      }
   if (is_char(val))
      {
      char32_t cp = std::get<char32_t>(val.repr);
      if (display_mode)
         { out << static_cast<char>(cp); return; }
      out << "#\\";
      switch (cp)
         {
         case 0x00: out << "nul";       break;
         case 0x07: out << "alarm";     break;
         case 0x08: out << "backspace"; break;
         case 0x09: out << "tab";       break;
         case 0x0A: out << "newline";   break;
         case 0x0D: out << "return";    break;
         case 0x1B: out << "escape";    break;
         case 0x20: out << "space";     break;
         case 0x7F: out << "delete";    break;
         default:
            if (cp > 0x20 && cp < 0x7F) out << static_cast<char>(cp);
            else out << "x" << std::hex << cp << std::dec;
            break;
         }
      return;
      }
   if (is_eof(val))       { out << "#<eof-object>"; return; }
   if (is_symbol(val))
      {
      const std::string& name = symbol_name(std::get<SymbolId>(val.repr).id);
      if (!display_mode)
         {
         bool needs_escape = name.empty();
         if (!needs_escape)
            for (char c : name)
               if (std::isspace(static_cast<unsigned char>(c)) ||
                   c=='('||c==')'||c=='"'||c==';'||c=='#'||c=='|'||c=='\\'||c=='\'')
                  { needs_escape = true; break; }
         if (needs_escape)
            {
            out << '|';
            for (char c : name)
               { if (c=='|') out << "\\|"; else if (c=='\\') out << "\\\\"; else out << c; }
            out << '|';
            return;
            }
         }
      out << name;
      return;
      }
   if (is_unspecified(val)){ out << "#<unspecified>"; return; }
   if (is_string(val))
      {
      const std::string& text = std::get<SchemeString*>(val.repr)->data;
      if (display_mode) out << text;
      else write_string_escaped(out, text);
      return;
      }
   if (is_builtin(val))   { out << "#<procedure:" << std::get<Builtin*>(val.repr)->name << ">"; return; }
   if (is_closure(val))
      {
      auto* cl = std::get<SchemeClosure*>(val.repr);
      out << "#<procedure"; if (!cl->name.empty()) out << ":" << cl->name; out << ">"; return;
      }
   if (is_case_closure(val))
      {
      auto* cc = std::get<SchemeCaseClosure*>(val.repr);
      out << "#<procedure"; if (!cc->name.empty()) out << ":" << cc->name; out << ">"; return;
      }
   if (is_macro(val))
      {
      auto* m = std::get<SchemeMacro*>(val.repr);
      out << "#<macro"; if (!m->name.empty()) out << ":" << m->name; out << ">"; return;
      }
   if (is_syntax_transformer(val))
      {
      auto* t = std::get<SchemeSyntaxTransformer*>(val.repr);
      out << "#<syntax-rules"; if (!t->name.empty()) out << ":" << t->name; out << ">"; return;
      }
   if (is_continuation(val)) { out << "#<continuation>"; return; }
   if (is_promise(val))
      {
      auto* p = std::get<SchemePromise*>(val.repr);
      out << (p->forced ? "#<promise:forced>" : "#<promise>"); return;
      }
   if (is_parameter(val)) { out << "#<parameter>"; return; }
   if (is_environment(val)){ out << "#<environment>"; return; }
   if (is_port(val))
      {
      auto* port = std::get<SchemePort*>(val.repr);
      if (!port->is_open)             out << "#<closed-port>";
      else if (port->is_input && port->is_output) out << "#<input-output-port>";
      else if (port->is_input)        out << "#<input-port>";
      else                            out << "#<output-port>";
      return;
      }
   if (is_vector(val))
      {
      auto* vec = std::get<SchemeVector*>(val.repr);
      out << "#(";
      for (size_t i = 0; i < vec->elements.size(); ++i)
         { if (i) out << " "; value_to_stream(out, vec->elements[i], display_mode); }
      out << ")"; return;
      }
   if (is_bytevector(val))
      {
      auto* bv = std::get<SchemeBytevector*>(val.repr);
      out << "#u8(";
      for (size_t i = 0; i < bv->data.size(); ++i)
         { if (i) out << " "; out << static_cast<int>(bv->data[i]); }
      out << ")"; return;
      }
   if (is_multi_values(val))
      {
      auto* mv = std::get<SchemeMultiValues*>(val.repr);
      out << "#<values";
      for (auto& v : mv->values) { out << " "; value_to_stream(out, v, display_mode); }
      out << ">"; return;
      }
   if (is_record(val))
      {
      auto* rec = std::get<SchemeRecord*>(val.repr);
      out << "#<record";
      if (rec->type) out << ":" << rec->type->name;
      out << ">"; return;
      }
   if (is_error_object(val))
      {
      auto* eo = std::get<SchemeErrorObject*>(val.repr);
      out << "#<error-object:" << eo->message << ">"; return;
      }
   if (is_cons(val))
      {
      out << "(";
      Value cur = val;
      bool first = true;
      while (is_cons(cur))
         {
         if (!first) out << " ";
         first = false;
         value_to_stream(out, car(cur), display_mode);
         cur = cdr(cur);
         }
      if (!is_nil(cur)) { out << " . "; value_to_stream(out, cur, display_mode); }
      out << ")"; return;
      }
   out << "#<unknown>";
   }

std::string value_to_string(Value val, bool display_mode)
   {
   std::ostringstream out;
   value_to_stream(out, val, display_mode);
   return out.str();
   }


// ── write-shared: two-pass cycle/sharing detector ─────────────────────────────

struct SharedCtx
   {
   std::unordered_map<void*, int> count;
   std::unordered_map<void*, int> labels;
   int next_id = 0;
   };

static void count_refs(Value val, SharedCtx& ctx)
   {
   if (is_cons(val))
      {
      void* ptr = std::get<ConsCell*>(val.repr);
      if (++(ctx.count[ptr]) > 1) return;
      count_refs(car(val), ctx);
      count_refs(cdr(val), ctx);
      }
   else if (is_vector(val))
      {
      void* ptr = std::get<SchemeVector*>(val.repr);
      if (++(ctx.count[ptr]) > 1) return;
      for (auto& e : std::get<SchemeVector*>(val.repr)->elements)
         count_refs(e, ctx);
      }
   }

static void print_shared_val(Value val, bool write_mode, SharedCtx& ctx, std::ostream& out);

static void print_shared_list_tail(Value val, bool write_mode, SharedCtx& ctx, std::ostream& out)
   {
   while (is_cons(val))
      {
      void* ptr = std::get<ConsCell*>(val.repr);
      if (ctx.count[ptr] > 1) { out << " . "; print_shared_val(val, write_mode, ctx, out); return; }
      out << " ";
      print_shared_val(car(val), write_mode, ctx, out);
      val = cdr(val);
      }
   if (!is_nil(val)) { out << " . "; print_shared_val(val, write_mode, ctx, out); }
   }

static void print_shared_val(Value val, bool write_mode, SharedCtx& ctx, std::ostream& out)
   {
   void* ptr = nullptr;
   if (is_cons(val))   ptr = std::get<ConsCell*>(val.repr);
   else if (is_vector(val)) ptr = std::get<SchemeVector*>(val.repr);

   if (ptr && ctx.count[ptr] > 1)
      {
      auto it = ctx.labels.find(ptr);
      if (it != ctx.labels.end()) { out << "#" << it->second << "#"; return; }
      int id = ctx.next_id++;
      ctx.labels[ptr] = id;
      out << "#" << id << "=";
      }

   if (is_cons(val))
      {
      out << "(";
      print_shared_val(car(val), write_mode, ctx, out);
      print_shared_list_tail(cdr(val), write_mode, ctx, out);
      out << ")"; return;
      }
   if (is_vector(val))
      {
      auto* vec = std::get<SchemeVector*>(val.repr);
      out << "#(";
      for (size_t i = 0; i < vec->elements.size(); ++i)
         { if (i) out << " "; print_shared_val(vec->elements[i], write_mode, ctx, out); }
      out << ")"; return;
      }
   value_to_stream(out, val, !write_mode);
   }

static void find_cyclic(Value val,
                        std::unordered_set<void*>& on_path,
                        std::unordered_set<void*>& visited,
                        std::unordered_set<void*>& cyclic)
   {
   void* ptr = nullptr;
   if (is_cons(val))        ptr = std::get<ConsCell*>(val.repr);
   else if (is_vector(val)) ptr = std::get<SchemeVector*>(val.repr);
   if (!ptr) return;
   if (visited.count(ptr)) return;
   if (on_path.count(ptr)) { cyclic.insert(ptr); return; }
   on_path.insert(ptr);
   if (is_cons(val))
      { find_cyclic(car(val), on_path, visited, cyclic); find_cyclic(cdr(val), on_path, visited, cyclic); }
   else
      for (auto& e : std::get<SchemeVector*>(val.repr)->elements)
         find_cyclic(e, on_path, visited, cyclic);
   on_path.erase(ptr);
   visited.insert(ptr);
   }

void write_to_stream(Value val, std::ostream& out)
   {
   std::unordered_set<void*> on_path, visited, cyclic;
   find_cyclic(val, on_path, visited, cyclic);
   SharedCtx ctx;
   for (void* p : cyclic) ctx.count[p] = 2;
   print_shared_val(val, true, ctx, out);
   }

void write_shared_to_stream(Value val, std::ostream& out)
   {
   SharedCtx ctx;
   count_refs(val, ctx);
   print_shared_val(val, true, ctx, out);
   }

std::string write_shared_to_string(Value val)
   {
   std::ostringstream out;
   write_shared_to_stream(val, out);
   return out.str();
   }


// ── Arithmetic helpers ────────────────────────────────────────────────────────

static bool both_fixnums(Value a, Value b)
   { return is_fixnum(a) && is_fixnum(b); }

static std::pair<double,double> complex_parts(Value v)
   {
   if (is_fixnum(v))   return {static_cast<double>(std::get<int64_t>(v.repr)), 0.0};
   if (is_flonum(v))   return {std::get<double>(v.repr), 0.0};
   if (is_rational(v)) return {to_double(v), 0.0};
   auto* c = std::get<SchemeComplex*>(v.repr);
   return {c->real, c->imag};
   }

static int64_t exact_num(Value v)
   { return is_fixnum(v) ? std::get<int64_t>(v.repr) : std::get<SchemeRational*>(v.repr)->num; }

static int64_t exact_den(Value v)
   { return is_fixnum(v) ? 1 : std::get<SchemeRational*>(v.repr)->den; }

static Value rat_make(int64_t n, int64_t d)
   {
   if (d == 0) throw SchemeDivisionError{};
   if (d < 0) { n = -n; d = -d; }
   int64_t g = std::gcd(n < 0 ? -n : n, d);
   n /= g; d /= g;
   if (d == 1) return make_fixnum(n);
   return make_rational(gc_alloc_rational(n, d));
   }

static Value normalize_complex(double r, double i)
   { return (i == 0.0) ? make_flonum(r) : make_complex_val(r, i); }

Value num_add(Value lhs, Value rhs)
   {
   if (both_fixnums(lhs, rhs))
      return make_fixnum(std::get<int64_t>(lhs.repr) + std::get<int64_t>(rhs.repr));
   if (is_exact_num(lhs) && is_exact_num(rhs))
      {
      int64_t a=exact_num(lhs), b=exact_den(lhs), c=exact_num(rhs), d=exact_den(rhs);
      int64_t g = std::gcd(b, d);
      return rat_make(a*(d/g)+c*(b/g), (b/g)*d);
      }
   if (is_complex(lhs) || is_complex(rhs))
      { auto[r1,i1]=complex_parts(lhs); auto[r2,i2]=complex_parts(rhs); return normalize_complex(r1+r2,i1+i2); }
   return make_flonum(to_double(lhs) + to_double(rhs));
   }

Value num_sub(Value lhs, Value rhs)
   {
   if (both_fixnums(lhs, rhs))
      return make_fixnum(std::get<int64_t>(lhs.repr) - std::get<int64_t>(rhs.repr));
   if (is_exact_num(lhs) && is_exact_num(rhs))
      {
      int64_t a=exact_num(lhs), b=exact_den(lhs), c=exact_num(rhs), d=exact_den(rhs);
      int64_t g = std::gcd(b, d);
      return rat_make(a*(d/g)-c*(b/g), (b/g)*d);
      }
   if (is_complex(lhs) || is_complex(rhs))
      { auto[r1,i1]=complex_parts(lhs); auto[r2,i2]=complex_parts(rhs); return normalize_complex(r1-r2,i1-i2); }
   return make_flonum(to_double(lhs) - to_double(rhs));
   }

Value num_mul(Value lhs, Value rhs)
   {
   if (both_fixnums(lhs, rhs))
      return make_fixnum(std::get<int64_t>(lhs.repr) * std::get<int64_t>(rhs.repr));
   if (is_exact_num(lhs) && is_exact_num(rhs))
      {
      int64_t a=exact_num(lhs), b=exact_den(lhs), c=exact_num(rhs), d=exact_den(rhs);
      int64_t g1=std::gcd(a<0?-a:a,d), g2=std::gcd(c<0?-c:c,b);
      return rat_make((a/g1)*(c/g2),(b/g2)*(d/g1));
      }
   if (is_complex(lhs) || is_complex(rhs))
      { auto[r1,i1]=complex_parts(lhs); auto[r2,i2]=complex_parts(rhs); return normalize_complex(r1*r2-i1*i2,r1*i2+i1*r2); }
   return make_flonum(to_double(lhs) * to_double(rhs));
   }

Value num_div(Value lhs, Value rhs)
   {
   if (both_fixnums(lhs, rhs))
      {
      int64_t divisor = std::get<int64_t>(rhs.repr);
      if (divisor == 0) throw SchemeDivisionError{};
      return rat_make(std::get<int64_t>(lhs.repr), divisor);
      }
   if (is_exact_num(lhs) && is_exact_num(rhs))
      {
      int64_t a=exact_num(lhs), b=exact_den(lhs), c=exact_num(rhs), d=exact_den(rhs);
      if (c == 0) throw SchemeDivisionError{};
      return rat_make(a*d, b*c);
      }
   if (is_complex(lhs) || is_complex(rhs))
      {
      auto[r1,i1]=complex_parts(lhs); auto[r2,i2]=complex_parts(rhs);
      double denom = r2*r2+i2*i2;
      if (denom == 0.0) throw SchemeDivisionError{};
      return normalize_complex((r1*r2+i1*i2)/denom,(i1*r2-r1*i2)/denom);
      }
   double div = to_double(rhs);
   if (div == 0.0) throw SchemeDivisionError{};
   return make_flonum(to_double(lhs) / div);
   }

static int rat_cmp(Value lhs, Value rhs)
   {
   int64_t a=exact_num(lhs), b=exact_den(lhs), c=exact_num(rhs), d=exact_den(rhs);
   int64_t lv=a*d, rv=c*b;
   if (lv<rv) return -1; if (lv>rv) return 1; return 0;
   }

bool num_lt(Value lhs, Value rhs)
   {
   if (is_complex(lhs)||is_complex(rhs)) throw SchemeError("order predicate undefined for complex numbers");
   if (is_exact_num(lhs)&&is_exact_num(rhs)) return rat_cmp(lhs,rhs)<0;
   return to_double(lhs)<to_double(rhs);
   }
bool num_le(Value lhs, Value rhs)
   {
   if (is_complex(lhs)||is_complex(rhs)) throw SchemeError("order predicate undefined for complex numbers");
   if (is_exact_num(lhs)&&is_exact_num(rhs)) return rat_cmp(lhs,rhs)<=0;
   return to_double(lhs)<=to_double(rhs);
   }
bool num_gt(Value lhs, Value rhs)
   {
   if (is_complex(lhs)||is_complex(rhs)) throw SchemeError("order predicate undefined for complex numbers");
   if (is_exact_num(lhs)&&is_exact_num(rhs)) return rat_cmp(lhs,rhs)>0;
   return to_double(lhs)>to_double(rhs);
   }
bool num_ge(Value lhs, Value rhs)
   {
   if (is_complex(lhs)||is_complex(rhs)) throw SchemeError("order predicate undefined for complex numbers");
   if (is_exact_num(lhs)&&is_exact_num(rhs)) return rat_cmp(lhs,rhs)>=0;
   return to_double(lhs)>=to_double(rhs);
   }
bool num_eq(Value lhs, Value rhs)
   {
   if (both_fixnums(lhs,rhs)) return std::get<int64_t>(lhs.repr)==std::get<int64_t>(rhs.repr);
   if (is_exact_num(lhs)&&is_exact_num(rhs)) return rat_cmp(lhs,rhs)==0;
   if (is_complex(lhs)||is_complex(rhs))
      { auto[r1,i1]=complex_parts(lhs); auto[r2,i2]=complex_parts(rhs); return r1==r2&&i1==i2; }
   return to_double(lhs)==to_double(rhs);
   }


// ── GC support for moving collectors ─────────────────────────────────────────

GcHeader* gc_value_header(Value val)
   {
   if (auto* p = std::get_if<ConsCell*>(&val.repr))            return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemeString*>(&val.repr))        return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemeClosure*>(&val.repr))       return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemeCaseClosure*>(&val.repr))   return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemeMacro*>(&val.repr))         return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemeSyntaxTransformer*>(&val.repr)) return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemeContinuation*>(&val.repr))  return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemeVector*>(&val.repr))        return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemeBytevector*>(&val.repr))    return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemePromise*>(&val.repr))       return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemeParameter*>(&val.repr))     return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemePort*>(&val.repr))          return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<Environment*>(&val.repr))         return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemeComplex*>(&val.repr))       return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemeRational*>(&val.repr))      return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemeMultiValues*>(&val.repr))   return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemeRecord*>(&val.repr))        return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemeExactComplex*>(&val.repr))  return *p ? &(*p)->header : nullptr;
   if (auto* p = std::get_if<SchemeErrorObject*>(&val.repr))   return *p ? &(*p)->header : nullptr;
   return nullptr;
   }

void gc_forward_value(Value& val)
   {
#define FWD(T) \
   if (auto* p = std::get_if<T*>(&val.repr)) \
      { if (*p && (*p)->header.forward) *p = reinterpret_cast<T*>((*p)->header.forward); return; }
   FWD(ConsCell)
   FWD(SchemeString)
   FWD(SchemeClosure)
   FWD(SchemeCaseClosure)
   FWD(SchemeMacro)
   FWD(SchemeSyntaxTransformer)
   FWD(SchemeContinuation)
   FWD(SchemeVector)
   FWD(SchemeBytevector)
   FWD(SchemePromise)
   FWD(SchemeParameter)
   FWD(SchemePort)
   FWD(Environment)
   FWD(SchemeComplex)
   FWD(SchemeRational)
   FWD(SchemeMultiValues)
   FWD(SchemeRecord)
   FWD(SchemeExactComplex)
   FWD(SchemeErrorObject)
#undef FWD
   }
