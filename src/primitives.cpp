#ifdef _WIN32
#  define _USE_MATH_DEFINES
#endif
#include "primitives.h"
#include "evaluator.h"
#include "expander.h"
#include "parser.h"
#include "analyzer.h"
#include "gc.h"
#include "port.h"
#include "exceptions.h"
#include "symbol.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <algorithm>
#include <cassert>
#include <unordered_map>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <codecvt>
#include <locale>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

// ── Static output stream for display/write/newline when no port is given ────────
static std::ostream* g_primitive_output = nullptr;

void set_primitive_output(std::ostream* out)
   {
   g_primitive_output = out;
   }

static std::ostream& prim_out()
   {
   return g_primitive_output ? *g_primitive_output : std::cout;
   }

// ── Builtin storage ───────────────────────────────────────────────────────────
// Primitives registered via def() are stored here (static lifetime).
// Dynamic builtins (record accessors/mutators) are stored separately.
static std::vector<std::unique_ptr<Builtin>>     g_builtins;
static std::unordered_map<std::string, Builtin*> g_builtin_by_name;
static std::vector<std::unique_ptr<Builtin>>     g_dynamic_builtins;

// ── Record type registry ──────────────────────────────────────────────────────
// SchemeRecordType has static lifetime (not GC-managed).
// We store unique_ptrs so pointers are stable.
static std::vector<std::unique_ptr<SchemeRecordType>> g_record_types;

// Command-line args
static std::vector<std::string> g_command_line_args;

// ── def() helper ─────────────────────────────────────────────────────────────
static void def(Environment* env, const char* name, BuiltinFn fn,
                int min_args = -1, int max_args = -1)
   {
   g_builtins.push_back(std::make_unique<Builtin>(name, std::move(fn)));
   Builtin* b = g_builtins.back().get();
   b->min_args = min_args;
   b->max_args = max_args;
   g_builtin_by_name[name] = b;
   env->bind(name, make_builtin(b));
   register_primitive_arity(name, min_args, max_args);
   }

// ── Error helpers ─────────────────────────────────────────────────────────────
[[noreturn]] static void raise_error(const std::string& msg,
                                      const std::vector<Value>& irritants = {},
                                      int kind = 0)
   {
   SchemeErrorObject* obj = gc_alloc_error_object();
   obj->message = msg;
   obj->kind    = kind;
   Value tail = make_nil();
   for (int i = (int)irritants.size() - 1; i >= 0; --i)
      {
      auto* c = gc_alloc_cons(); c->car = irritants[i]; c->cdr = tail;
      tail = make_cons(c);
      }
   obj->irritants = tail;
   throw SchemeRaisedException(make_error_object_val(obj), false);
   }

[[noreturn]] static void raise_file_error(const std::string& msg)
   { raise_error(msg, {}, 1); }

[[noreturn]] static void raise_read_error(const std::string& msg)
   { raise_error(msg, {}, 2); }

// ── Multi-values helper ───────────────────────────────────────────────────────
static Value make_values_val(std::vector<Value> vals)
   {
   if (vals.size() == 1) return vals[0];
   SchemeMultiValues* mv = gc_alloc_multi_values();
   mv->values = std::move(vals);
   return make_multi_values_val(mv);
   }

// ── Record type helpers ───────────────────────────────────────────────────────
static bool is_record_type_descriptor(Value v)
   {
   return is_record(v) && as_record(v)->type == nullptr;
   }

static SchemeRecordType* unwrap_record_type(Value v, const char* who)
   {
   if (!is_record_type_descriptor(v))
      throw SchemeTypeError(std::string(who) + ": argument is not a record type");
   int64_t idx = as_fixnum(as_record(v)->fields[0]);
   return g_record_types[(size_t)idx].get();
   }

static Value alloc_record_type_descriptor(const std::string& name,
                                           const std::vector<std::string>& fields)
   {
   size_t idx = g_record_types.size();
   g_record_types.push_back(std::make_unique<SchemeRecordType>(name, fields));
   SchemeRecord* r = gc_alloc_record();
   r->type = nullptr;   // sentinel: type descriptor
   r->fields.push_back(make_fixnum((int64_t)idx));
   return make_record_val(r);
   }

// ── flonum -> exact conversion ────────────────────────────────────────────────
static Value flonum_to_exact(double f)
   {
   if (!std::isfinite(f))
      throw SchemeError("exact: no exact representation for infinity/NaN");
   if (f == std::trunc(f))
      return make_fixnum((int64_t)f);
   int exp;
   double mant = std::frexp(f, &exp);
   int64_t int_mant = (int64_t)std::ldexp(std::abs(mant), 53);
   if (f < 0.0) int_mant = -int_mant;
   int adj = exp - 53;
   if (adj >= 0)
      {
      if (adj < 62) return make_fixnum(int_mant << adj);
      // overflow: fall back to float
      return make_flonum(f);
      }
   int neg = -adj;
   if (neg > 62) throw SchemeError("exact: float too small to represent exactly");
   int64_t g = std::gcd(int_mant < 0 ? -int_mant : int_mant, int64_t(1) << neg);
   int64_t n = int_mant / g;
   int64_t d = (int64_t(1) << neg) / g;
   if (d == 1) return make_fixnum(n);
   return make_rational(gc_alloc_rational(n, d));
   }

// ── to_double for any real ────────────────────────────────────────────────────
static double scheme_to_double(Value v)
   {
   if (is_fixnum(v))    return (double)as_fixnum(v);
   if (is_flonum(v))    return as_flonum(v);
   if (is_rational(v))  { auto* r = as_rational(v); return (double)r->num / (double)r->den; }
   if (is_complex(v))   { auto* c = as_complex(v); if (c->imag == 0.0) return c->real; }
   throw SchemeTypeError("not a real number", value_to_string(v));
   }

// ── Rationalize helpers ───────────────────────────────────────────────────────
// Returns the simplest rational with numerator/denominator in range [lo,hi].
// lo and hi are doubles with lo <= hi and both in the same sign.
static std::pair<int64_t,int64_t> simplest_rational_positive(double lo, double hi)
   {
   // Stern-Brocot / mediants
   int64_t ln = 0, ld = 1;  // lower bound: ln/ld
   int64_t hn = 1, hd = 0;  // upper bound: hn/hd (1/0 = +inf)
   while (true)
      {
      int64_t mn = ln + hn, md = ld + hd;  // mediant
      double  mv = (double)mn / (double)md;
      if (mv < lo)      { ln = mn; ld = md; }
      else if (mv > hi) { hn = mn; hd = md; }
      else               return {mn, md};
      // Safety: if we've overflowed, just return the nearest bound
      if (mn < 0 || md < 0) break;
      }
   // Fallback
   int64_t n = (int64_t)std::round(lo);
   return {n, 1};
   }

// ── UTF-8 helpers ─────────────────────────────────────────────────────────────
// Encode a Unicode codepoint to UTF-8 bytes.
static std::string codepoint_to_utf8(char32_t cp)
   {
   std::string out;
   if (cp < 0x80)
      { out += (char)cp; }
   else if (cp < 0x800)
      { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
   else if (cp < 0x10000)
      { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
   else
      { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
   return out;
   }

// Decode one UTF-8 sequence from a string at position pos. Advances pos.
static char32_t utf8_decode_one(const std::string& s, size_t& pos)
   {
   unsigned char c = (unsigned char)s[pos++];
   if (c < 0x80) return (char32_t)c;
   int extra = 0;
   char32_t cp = 0;
   if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
   else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
   else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
   else return (char32_t)c;  // invalid: treat as latin-1
   while (extra-- > 0 && pos < s.size())
      cp = (cp << 6) | ((unsigned char)s[pos++] & 0x3F);
   return cp;
   }

// Count Unicode codepoints in a UTF-8 string.
static size_t utf8_codepoint_count(const std::string& s)
   {
   size_t count = 0;
   size_t pos = 0;
   while (pos < s.size()) { utf8_decode_one(s, pos); ++count; }
   return count;
   }

// Get codepoint at index k in a UTF-8 string.
static char32_t utf8_codepoint_at(const std::string& s, size_t k)
   {
   size_t pos = 0;
   for (size_t i = 0; i < k; ++i) utf8_decode_one(s, pos);
   return utf8_decode_one(s, pos);
   }

// Replace codepoint at index k in a UTF-8 string.
static std::string utf8_set_at(const std::string& s, size_t k, char32_t cp)
   {
   std::string result;
   size_t pos = 0;
   size_t idx = 0;
   while (pos < s.size())
      {
      size_t start = pos;
      char32_t c = utf8_decode_one(s, pos);
      if (idx == k)
         result += codepoint_to_utf8(cp);
      else
         result += s.substr(start, pos - start);
      ++idx;
      }
   return result;
   }

// ── Port read helpers ─────────────────────────────────────────────────────────
static SchemePort* check_open_input_port(Value v, const char* who)
   {
   if (!is_port(v)) throw SchemeTypeError(std::string(who) + ": not a port");
   SchemePort* p = as_port(v);
   if (!p->is_open)  throw SchemeError(std::string(who) + ": port is closed");
   if (!p->is_input) throw SchemeTypeError(std::string(who) + ": not an input port");
   return p;
   }

static SchemePort* check_open_output_port(Value v, const char* who)
   {
   if (!is_port(v)) throw SchemeTypeError(std::string(who) + ": not a port");
   SchemePort* p = as_port(v);
   if (!p->is_open)   throw SchemeError(std::string(who) + ": port is closed");
   if (!p->is_output) throw SchemeTypeError(std::string(who) + ": not an output port");
   return p;
   }

// Read one Unicode codepoint from a textual input port. Returns -1 on EOF.
static int32_t port_read_char(SchemePort* p)
   {
   if (p->peek_val >= 0) { int32_t ch = p->peek_val; p->peek_val = -1; return ch; }
   if (p->kind == SchemePort::Kind::StrIn)
      {
      if (p->str_pos >= p->str_buf.size()) return -1;
      char32_t cp = utf8_decode_one(p->str_buf, p->str_pos);
      return (int32_t)cp;
      }
   // File/Stdio: read raw bytes and decode UTF-8
   if (!p->fp) return -1;
   int c = fgetc(p->fp);
   if (c == EOF) return -1;
   unsigned char first = (unsigned char)c;
   if (first < 0x80) return (int32_t)first;
   int extra = 0;
   char32_t cp = 0;
   if      ((first & 0xE0) == 0xC0) { cp = first & 0x1F; extra = 1; }
   else if ((first & 0xF0) == 0xE0) { cp = first & 0x0F; extra = 2; }
   else if ((first & 0xF8) == 0xF0) { cp = first & 0x07; extra = 3; }
   else return (int32_t)first;
   for (int i = 0; i < extra; ++i)
      {
      int next = fgetc(p->fp);
      if (next == EOF) break;
      cp = (cp << 6) | ((unsigned char)next & 0x3F);
      }
   return (int32_t)cp;
   }

static int32_t port_peek_char(SchemePort* p)
   {
   if (p->peek_val >= 0) return p->peek_val;
   int32_t ch = port_read_char(p);
   p->peek_val = ch;
   return ch;
   }

// Read one byte from a binary input port. Returns -1 on EOF.
static int port_read_byte(SchemePort* p)
   {
   if (p->peek_byte >= 0) { int b = p->peek_byte; p->peek_byte = -1; return b; }
   if (p->kind == SchemePort::Kind::BvIn)
      {
      if (p->bv_pos >= p->bv_buf.size()) return -1;
      return (int)(unsigned char)p->bv_buf[p->bv_pos++];
      }
   if (!p->fp) return -1;
   int b = fgetc(p->fp);
   return b;  // EOF returns -1 as well
   }

static int port_peek_byte(SchemePort* p)
   {
   if (p->peek_byte >= 0) return p->peek_byte;
   int b = port_read_byte(p);
   p->peek_byte = b;
   return b;
   }

// Write a UTF-8 encoded codepoint to a textual output port.
static void port_write_char(SchemePort* p, char32_t cp)
   {
   std::string utf8 = codepoint_to_utf8(cp);
   if (p->kind == SchemePort::Kind::StrOut || p->kind == SchemePort::Kind::Stdio)
      {
      if (p->kind == SchemePort::Kind::StrOut)
         p->out_buf += utf8;
      else
         {
         if (p->fp) { fwrite(utf8.data(), 1, utf8.size(), p->fp); fflush(p->fp); }
         else         prim_out() << utf8 << std::flush;
         }
      return;
      }
   if (p->fp) { fwrite(utf8.data(), 1, utf8.size(), p->fp); }
   }

static void port_write_string(SchemePort* p, const std::string& s)
   {
   if (p->kind == SchemePort::Kind::StrOut)
      { p->out_buf += s; return; }
   if (p->kind == SchemePort::Kind::Stdio)
      {
      if (p->fp) { fwrite(s.data(), 1, s.size(), p->fp); fflush(p->fp); }
      else         prim_out() << s << std::flush;
      return;
      }
   if (p->fp) fwrite(s.data(), 1, s.size(), p->fp);
   }

// Global current-port parameters (built-in initial ports)
static SchemePort* make_stdio_port(bool is_input, FILE* fp, const char* name)
   {
   SchemePort* p = gc_alloc_port();
   p->is_input   = is_input;
   p->is_output  = !is_input;
   p->is_text    = true;
   p->is_open    = true;
   p->kind       = SchemePort::Kind::Stdio;
   p->fp         = fp;
   p->owns_fp    = false;
   p->name       = name;
   return p;
   }

static Value g_current_input_port;
static Value g_current_output_port;
static Value g_current_error_port;

static SchemePort* current_input_port_raw()
   {
   if (is_parameter(g_current_input_port))
      {
      Value cur = as_parameter(g_current_input_port)->current;
      if (is_port(cur)) return as_port(cur);
      }
   return nullptr;
   }

static SchemePort* current_output_port_raw(CekCtx* ctx)
   {
   // If ctx has an output stream, return a transient port for it.
   // The caller holds ctx, so the port is valid for the call duration.
   if (is_parameter(g_current_output_port))
      {
      Value cur = as_parameter(g_current_output_port)->current;
      if (is_port(cur)) return as_port(cur);
      }
   return nullptr;
   }

// ── Arithmetic section ────────────────────────────────────────────────────────
static void register_arithmetic(Environment* env)
   {
   def(env, "+", [](ArgVec args, CekCtx*) -> Value
      {
      Value acc = make_fixnum(0);
      for (auto& a : args) acc = num_add(acc, a);
      return acc;
      }, 0, -1);

   def(env, "-", [](ArgVec args, CekCtx*) -> Value
      {
      if (args.size() == 1)
         {
         if (is_fixnum(args[0]))  return make_fixnum(-as_fixnum(args[0]));
         if (is_complex(args[0])) { auto* c = as_complex(args[0]); return make_complex_val(-c->real, -c->imag); }
         if (is_rational(args[0])) { auto* r = as_rational(args[0]); return make_rational(gc_alloc_rational(-r->num, r->den)); }
         if (is_exact_complex(args[0]))
            {
            auto* ec = as_exact_complex(args[0]);
            Value nr = num_sub(make_fixnum(0), ec->real);
            Value ni = num_sub(make_fixnum(0), ec->imag);
            return make_exact_complex_val(gc_alloc_exact_complex(nr, ni));
            }
         return make_flonum(-as_flonum(args[0]));
         }
      Value acc = args[0];
      for (size_t i = 1; i < args.size(); ++i) acc = num_sub(acc, args[i]);
      return acc;
      }, 1, -1);

   def(env, "*", [](ArgVec args, CekCtx*) -> Value
      {
      Value acc = make_fixnum(1);
      for (auto& a : args) acc = num_mul(acc, a);
      return acc;
      }, 0, -1);

   def(env, "/", [](ArgVec args, CekCtx*) -> Value
      {
      if (args.size() == 1) return num_div(make_fixnum(1), args[0]);
      Value acc = args[0];
      for (size_t i = 1; i < args.size(); ++i) acc = num_div(acc, args[i]);
      return acc;
      }, 1, -1);

   def(env, "abs", [](ArgVec args, CekCtx*) -> Value
      {
      if (is_fixnum(args[0]))  { int64_t n = as_fixnum(args[0]); return make_fixnum(n < 0 ? -n : n); }
      if (is_rational(args[0])) { auto* r = as_rational(args[0]); return make_rational(gc_alloc_rational(r->num < 0 ? -r->num : r->num, r->den)); }
      if (is_complex(args[0])) { auto* c = as_complex(args[0]); return make_flonum(std::hypot(c->real, c->imag)); }
      if (is_exact_complex(args[0]))
         {
         auto* ec = as_exact_complex(args[0]);
         double re = scheme_to_double(ec->real), im = scheme_to_double(ec->imag);
         return make_flonum(std::hypot(re, im));
         }
      return make_flonum(std::abs(as_flonum(args[0])));
      }, 1, 1);

   def(env, "quotient", [](ArgVec args, CekCtx*) -> Value
      {
      bool inexact = false;
      auto to_int = [&](Value v, const char* who) -> int64_t
         {
         if (is_fixnum(v)) return as_fixnum(v);
         if (is_flonum(v)) { double d = as_flonum(v); if (d == std::trunc(d)) { inexact = true; return (int64_t)d; } }
         throw SchemeError(std::string(who) + ": not an integer");
         };
      int64_t n = to_int(args[0], "quotient");
      int64_t d = to_int(args[1], "quotient");
      if (d == 0) throw SchemeDivisionError{};
      int64_t q = n / d;
      return inexact ? make_flonum((double)q) : make_fixnum(q);
      }, 2, 2);

   def(env, "remainder", [](ArgVec args, CekCtx*) -> Value
      {
      bool inexact = false;
      auto to_int = [&](Value v, const char* who) -> int64_t
         {
         if (is_fixnum(v)) return as_fixnum(v);
         if (is_flonum(v)) { double d = as_flonum(v); if (d == std::trunc(d)) { inexact = true; return (int64_t)d; } }
         throw SchemeError(std::string(who) + ": not an integer");
         };
      int64_t n = to_int(args[0], "remainder");
      int64_t d = to_int(args[1], "remainder");
      if (d == 0) throw SchemeDivisionError{};
      int64_t r = n % d;
      return inexact ? make_flonum((double)r) : make_fixnum(r);
      }, 2, 2);

   def(env, "modulo", [](ArgVec args, CekCtx*) -> Value
      {
      bool inexact = false;
      auto to_int = [&](Value v, const char* who) -> int64_t
         {
         if (is_fixnum(v)) return as_fixnum(v);
         if (is_flonum(v)) { double d = as_flonum(v); if (d == std::trunc(d)) { inexact = true; return (int64_t)d; } }
         throw SchemeError(std::string(who) + ": not an integer");
         };
      int64_t n = to_int(args[0], "modulo");
      int64_t d = to_int(args[1], "modulo");
      if (d == 0) throw SchemeDivisionError{};
      int64_t r = n % d;
      if (r != 0 && (r < 0) != (d < 0)) r += d;
      return inexact ? make_flonum((double)r) : make_fixnum(r);
      }, 2, 2);

   def(env, "min", [](ArgVec args, CekCtx*) -> Value
      {
      Value   result     = args[0];
      bool    any_inexact = is_flonum(args[0]);
      for (size_t i = 1; i < args.size(); ++i)
         {
         if (is_flonum(args[i])) any_inexact = true;
         if (is_flonum(args[i]) && std::isnan(as_flonum(args[i]))) { result = args[i]; }
         else if (!(is_flonum(result) && std::isnan(as_flonum(result))))
            { if (num_lt(args[i], result)) result = args[i]; }
         }
      if (any_inexact && is_exact_num(result)) return make_flonum(scheme_to_double(result));
      return result;
      }, 1, -1);

   def(env, "max", [](ArgVec args, CekCtx*) -> Value
      {
      Value result      = args[0];
      bool  any_inexact = is_flonum(args[0]);
      for (size_t i = 1; i < args.size(); ++i)
         {
         if (is_flonum(args[i])) any_inexact = true;
         if (is_flonum(args[i]) && std::isnan(as_flonum(args[i]))) { result = args[i]; }
         else if (!(is_flonum(result) && std::isnan(as_flonum(result))))
            { if (num_gt(args[i], result)) result = args[i]; }
         }
      if (any_inexact && is_exact_num(result)) return make_flonum(scheme_to_double(result));
      return result;
      }, 1, -1);

   def(env, "gcd", [](ArgVec args, CekCtx*) -> Value
      {
      if (args.empty()) return make_fixnum(0);
      bool inexact = false;
      int64_t g = 0;
      for (auto& a : args)
         {
         int64_t v;
         if (is_fixnum(a)) v = std::abs(as_fixnum(a));
         else if (is_flonum(a))
            {
            double d = as_flonum(a);
            if (d != std::trunc(d)) throw SchemeError("gcd: not an integer");
            inexact = true; v = (int64_t)std::abs(d);
            }
         else throw SchemeError("gcd: not an integer");
         g = std::gcd(g, v);
         }
      return inexact ? make_flonum((double)g) : make_fixnum(g);
      }, 0, -1);

   def(env, "lcm", [](ArgVec args, CekCtx*) -> Value
      {
      if (args.empty()) return make_fixnum(1);
      bool inexact = false;
      int64_t l = 1;
      for (auto& a : args)
         {
         int64_t v;
         if (is_fixnum(a)) v = std::abs(as_fixnum(a));
         else if (is_flonum(a))
            {
            double d = as_flonum(a);
            if (d != std::trunc(d)) throw SchemeError("lcm: not an integer");
            inexact = true; v = (int64_t)std::abs(d);
            }
         else throw SchemeError("lcm: not an integer");
         if (v == 0) { l = 0; }
         else if (l != 0) { l = l / std::gcd(l, v) * v; }
         }
      return inexact ? make_flonum((double)l) : make_fixnum(l);
      }, 0, -1);

   def(env, "expt", [](ArgVec args, CekCtx*) -> Value
      {
      Value base = args[0]; Value expo = args[1];
      // Complex expt
      if (is_complex(base) || is_complex(expo) || is_exact_complex(base) || is_exact_complex(expo))
         {
         double br = 0, bi = 0, er = 0, ei = 0;
         if (is_complex(base)) { br = as_complex(base)->real; bi = as_complex(base)->imag; }
         else if (is_exact_complex(base)) { br = scheme_to_double(as_exact_complex(base)->real); bi = scheme_to_double(as_exact_complex(base)->imag); }
         else { br = scheme_to_double(base); }
         if (is_complex(expo)) { er = as_complex(expo)->real; ei = as_complex(expo)->imag; }
         else if (is_exact_complex(expo)) { er = scheme_to_double(as_exact_complex(expo)->real); ei = scheme_to_double(as_exact_complex(expo)->imag); }
         else { er = scheme_to_double(expo); }
         std::complex<double> result = std::pow(std::complex<double>(br, bi), std::complex<double>(er, ei));
         if (result.imag() == 0.0) return make_flonum(result.real());
         return make_complex_val(result.real(), result.imag());
         }
      // Exact integer base, fixnum exponent
      if (is_exact_num(base) && is_fixnum(expo))
         {
         int64_t e = as_fixnum(expo);
         int64_t bn = is_rational(base) ? as_rational(base)->num : as_fixnum(base);
         int64_t bd = is_rational(base) ? as_rational(base)->den : 1;
         if (e == 0) return make_fixnum(1);
         if (bn == 0) { if (e < 0) throw SchemeError("expt: 0 to negative exponent"); return make_fixnum(0); }
         if (e < 0) { std::swap(bn, bd); e = -e; }
         auto ipow = [](int64_t b, int64_t ex, int64_t& out) -> bool
            {
            out = 1;
            while (ex > 0)
               {
               if (ex & 1) { double d = (double)out * (double)b; if (d > 9.2e18) return false; out *= b; }
               ex >>= 1;
               if (ex > 0) { double d = (double)b * (double)b; if (d > 9.2e18) return false; b *= b; }
               }
            return true;
            };
         int64_t np, dp;
         if (ipow(bn, e, np) && ipow(bd, e, dp))
            {
            if (dp < 0) { np = -np; dp = -dp; }
            int64_t g = std::gcd(np < 0 ? -np : np, dp);
            np /= g; dp /= g;
            if (dp == 1) return make_fixnum(np);
            return make_rational(gc_alloc_rational(np, dp));
            }
         }
      return make_flonum(std::pow(scheme_to_double(base), scheme_to_double(expo)));
      }, 2, 2);

   def(env, "sqrt", [](ArgVec args, CekCtx*) -> Value
      {
      if (is_complex(args[0]))
         {
         auto* c = as_complex(args[0]);
         std::complex<double> r = std::sqrt(std::complex<double>(c->real, c->imag));
         if (r.imag() == 0.0) return make_flonum(r.real());
         return make_complex_val(r.real(), r.imag());
         }
      if (is_exact_complex(args[0]))
         {
         auto* ec = as_exact_complex(args[0]);
         std::complex<double> r = std::sqrt(std::complex<double>(scheme_to_double(ec->real), scheme_to_double(ec->imag)));
         if (r.imag() == 0.0) return make_flonum(r.real());
         return make_complex_val(r.real(), r.imag());
         }
      if (is_fixnum(args[0]))
         {
         int64_t n = as_fixnum(args[0]);
         if (n >= 0) { int64_t s = (int64_t)std::sqrt((double)n); while (s > 0 && s * s > n) --s; while ((s+1)*(s+1) <= n) ++s; if (s * s == n) return make_fixnum(s); }
         if (n < 0) { return make_complex_val(0.0, std::sqrt((double)(-n))); }
         }
      if (is_rational(args[0]))
         {
         auto* r = as_rational(args[0]);
         if (r->num >= 0)
            {
            int64_t sn = (int64_t)std::sqrt((double)r->num); while (sn > 0 && sn*sn > r->num) --sn; while ((sn+1)*(sn+1) <= r->num) ++sn;
            int64_t sd = (int64_t)std::sqrt((double)r->den); while (sd > 0 && sd*sd > r->den) --sd; while ((sd+1)*(sd+1) <= r->den) ++sd;
            if (sn*sn == r->num && sd*sd == r->den) { int64_t g = std::gcd(sn, sd); sn /= g; sd /= g; if (sd == 1) return make_fixnum(sn); return make_rational(gc_alloc_rational(sn, sd)); }
            }
         }
      double x = scheme_to_double(args[0]);
      if (x < 0.0) return make_complex_val(0.0, std::sqrt(-x));
      return make_flonum(std::sqrt(x));
      }, 1, 1);

   def(env, "square",   [](ArgVec args, CekCtx*) -> Value { return num_mul(args[0], args[0]); }, 1, 1);

   def(env, "floor", [](ArgVec args, CekCtx*) -> Value
      {
      if (is_fixnum(args[0])) return args[0];
      if (is_rational(args[0])) { auto* r = as_rational(args[0]); int64_t q = r->num / r->den; if (r->num % r->den != 0 && r->num < 0) q--; return make_fixnum(q); }
      double x = as_flonum(args[0]);
      if (!std::isfinite(x)) return args[0];
      return make_flonum(std::floor(x));
      }, 1, 1);

   def(env, "ceiling", [](ArgVec args, CekCtx*) -> Value
      {
      if (is_fixnum(args[0])) return args[0];
      if (is_rational(args[0])) { auto* r = as_rational(args[0]); int64_t q = r->num / r->den; if (r->num % r->den != 0 && r->num > 0) q++; return make_fixnum(q); }
      double x = as_flonum(args[0]);
      if (!std::isfinite(x)) return args[0];
      return make_flonum(std::ceil(x));
      }, 1, 1);

   def(env, "truncate", [](ArgVec args, CekCtx*) -> Value
      {
      if (is_fixnum(args[0])) return args[0];
      if (is_rational(args[0])) { auto* r = as_rational(args[0]); return make_fixnum(r->num / r->den); }
      double x = as_flonum(args[0]);
      if (!std::isfinite(x)) return args[0];
      return make_flonum(std::trunc(x));
      }, 1, 1);

   def(env, "round", [](ArgVec args, CekCtx*) -> Value
      {
      if (is_fixnum(args[0])) return args[0];
      if (is_rational(args[0]))
         {
         auto* r = as_rational(args[0]);
         int64_t q = r->num / r->den;
         int64_t rem = std::abs(r->num % r->den);
         int64_t twice = 2 * rem;
         if (twice < r->den) return make_fixnum(q);
         if (twice > r->den) return make_fixnum(r->num > 0 ? q + 1 : q - 1);
         return make_fixnum(q % 2 == 0 ? q : (r->num > 0 ? q + 1 : q - 1));
         }
      double x = as_flonum(args[0]);
      if (!std::isfinite(x)) return args[0];
      double fl = std::floor(x);
      double diff = x - fl;
      if (diff < 0.5) return make_flonum(fl);
      if (diff > 0.5) return make_flonum(fl + 1.0);
      return make_flonum(std::fmod(fl, 2.0) == 0.0 ? fl : fl + 1.0);
      }, 1, 1);

   def(env, "exact?", [](ArgVec args, CekCtx*) -> Value
      { return make_bool(is_exact_num(args[0]) || is_exact_complex(args[0])); }, 1, 1);

   def(env, "inexact?", [](ArgVec args, CekCtx*) -> Value
      { return make_bool(is_flonum(args[0]) || is_complex(args[0])); }, 1, 1);

   def(env, "exact", [](ArgVec args, CekCtx*) -> Value
      {
      if (is_exact_num(args[0]) || is_exact_complex(args[0])) return args[0];
      if (is_flonum(args[0])) return flonum_to_exact(as_flonum(args[0]));
      if (is_complex(args[0]))
         {
         auto* c = as_complex(args[0]);
         return make_exact_complex_val(gc_alloc_exact_complex(flonum_to_exact(c->real), flonum_to_exact(c->imag)));
         }
      throw SchemeError("exact: argument must be a number");
      }, 1, 1);

   def(env, "inexact", [](ArgVec args, CekCtx*) -> Value
      {
      if (is_flonum(args[0]) || is_complex(args[0])) return args[0];
      if (is_fixnum(args[0]))  return make_flonum((double)as_fixnum(args[0]));
      if (is_rational(args[0])) { auto* r = as_rational(args[0]); return make_flonum((double)r->num / (double)r->den); }
      if (is_exact_complex(args[0]))
         {
         auto* ec = as_exact_complex(args[0]);
         return make_complex_val(scheme_to_double(ec->real), scheme_to_double(ec->imag));
         }
      throw SchemeError("inexact: argument must be a number");
      }, 1, 1);

   // Aliases
   def(env, "exact->inexact", [](ArgVec args, CekCtx*) -> Value
      {
      if (is_flonum(args[0]) || is_complex(args[0])) return args[0];
      return make_flonum(scheme_to_double(args[0]));
      }, 1, 1);
   def(env, "inexact->exact", [](ArgVec args, CekCtx*) -> Value
      {
      if (is_exact_num(args[0]) || is_exact_complex(args[0])) return args[0];
      if (is_flonum(args[0])) return flonum_to_exact(as_flonum(args[0]));
      throw SchemeError("inexact->exact: no exact representation for complex");
      }, 1, 1);

   def(env, "exact-integer?", [](ArgVec args, CekCtx*) -> Value
      { return make_bool(is_fixnum(args[0])); }, 1, 1);

   def(env, "numerator", [](ArgVec args, CekCtx*) -> Value
      {
      Value v = args[0];
      if (is_fixnum(v))    return v;
      if (is_rational(v))  return make_fixnum(as_rational(v)->num);
      if (is_flonum(v))
         {
         double f = as_flonum(v);
         if (!std::isfinite(f) || f == std::trunc(f)) return make_flonum(f);
         return make_flonum((double)(int64_t)flonum_to_exact(f).repr.index());  // fallback
         }
      throw SchemeError("numerator: argument must be a real number");
      }, 1, 1);

   def(env, "denominator", [](ArgVec args, CekCtx*) -> Value
      {
      Value v = args[0];
      if (is_fixnum(v))    return make_fixnum(1);
      if (is_rational(v))  return make_fixnum(as_rational(v)->den);
      if (is_flonum(v))
         {
         double f = as_flonum(v);
         if (!std::isfinite(f)) return make_flonum(1.0);
         if (f == std::trunc(f)) return make_flonum(1.0);
         return make_flonum(1.0);  // approximate; exact conversion is complex
         }
      throw SchemeError("denominator: argument must be a real number");
      }, 1, 1);

   def(env, "floor-quotient", [](ArgVec args, CekCtx*) -> Value
      {
      bool inx = false;
      auto to_int = [&](Value v) -> int64_t
         { if (is_fixnum(v)) return as_fixnum(v); if (is_flonum(v)) { inx = true; return (int64_t)as_flonum(v); } throw SchemeError("floor-quotient: not an integer"); };
      int64_t n = to_int(args[0]), d = to_int(args[1]);
      if (d == 0) throw SchemeDivisionError{};
      int64_t q = n / d;
      if ((n ^ d) < 0 && n % d != 0) q--;  // floor toward -inf
      return inx ? make_flonum((double)q) : make_fixnum(q);
      }, 2, 2);

   def(env, "floor-remainder", [](ArgVec args, CekCtx*) -> Value
      {
      bool inx = false;
      auto to_int = [&](Value v) -> int64_t
         { if (is_fixnum(v)) return as_fixnum(v); if (is_flonum(v)) { inx = true; return (int64_t)as_flonum(v); } throw SchemeError("floor-remainder: not an integer"); };
      int64_t n = to_int(args[0]), d = to_int(args[1]);
      if (d == 0) throw SchemeDivisionError{};
      int64_t r = n % d;
      if ((n < 0) != (d < 0) && r != 0) r += d;
      return inx ? make_flonum((double)r) : make_fixnum(r);
      }, 2, 2);

   def(env, "floor/", [](ArgVec args, CekCtx*) -> Value
      {
      bool inx = false;
      auto to_int = [&](Value v) -> int64_t
         { if (is_fixnum(v)) return as_fixnum(v); if (is_flonum(v)) { inx = true; return (int64_t)as_flonum(v); } throw SchemeError("floor/: not an integer"); };
      int64_t n = to_int(args[0]), d = to_int(args[1]);
      if (d == 0) throw SchemeDivisionError{};
      int64_t q = n / d; int64_t r = n % d;
      if ((n < 0) != (d < 0) && r != 0) { q--; r += d; }
      if (inx) return make_values_val({make_flonum((double)q), make_flonum((double)r)});
      return make_values_val({make_fixnum(q), make_fixnum(r)});
      }, 2, 2);

   def(env, "truncate-quotient", [](ArgVec args, CekCtx*) -> Value
      {
      bool inx = false;
      auto to_int = [&](Value v) -> int64_t
         { if (is_fixnum(v)) return as_fixnum(v); if (is_flonum(v)) { inx = true; return (int64_t)as_flonum(v); } throw SchemeError("truncate-quotient: not an integer"); };
      int64_t n = to_int(args[0]), d = to_int(args[1]);
      if (d == 0) throw SchemeDivisionError{};
      return inx ? make_flonum((double)(n/d)) : make_fixnum(n/d);
      }, 2, 2);

   def(env, "truncate-remainder", [](ArgVec args, CekCtx*) -> Value
      {
      bool inx = false;
      auto to_int = [&](Value v) -> int64_t
         { if (is_fixnum(v)) return as_fixnum(v); if (is_flonum(v)) { inx = true; return (int64_t)as_flonum(v); } throw SchemeError("truncate-remainder: not an integer"); };
      int64_t n = to_int(args[0]), d = to_int(args[1]);
      if (d == 0) throw SchemeDivisionError{};
      return inx ? make_flonum((double)(n%d)) : make_fixnum(n%d);
      }, 2, 2);

   def(env, "truncate/", [](ArgVec args, CekCtx*) -> Value
      {
      bool inx = false;
      auto to_int = [&](Value v) -> int64_t
         { if (is_fixnum(v)) return as_fixnum(v); if (is_flonum(v)) { inx = true; return (int64_t)as_flonum(v); } throw SchemeError("truncate/: not an integer"); };
      int64_t n = to_int(args[0]), d = to_int(args[1]);
      if (d == 0) throw SchemeDivisionError{};
      if (inx) return make_values_val({make_flonum((double)(n/d)), make_flonum((double)(n%d))});
      return make_values_val({make_fixnum(n/d), make_fixnum(n%d)});
      }, 2, 2);

   def(env, "exact-integer-sqrt", [](ArgVec args, CekCtx*) -> Value
      {
      if (!is_fixnum(args[0])) throw SchemeError("exact-integer-sqrt: argument must be an exact integer");
      int64_t n = as_fixnum(args[0]);
      if (n < 0) throw SchemeError("exact-integer-sqrt: argument must be non-negative");
      int64_t s = (int64_t)std::sqrt((double)n);
      while (s > 0 && s * s > n) --s;
      while ((s+1)*(s+1) <= n) ++s;
      return make_values_val({make_fixnum(s), make_fixnum(n - s*s)});
      }, 1, 1);

   // Transcendentals
   def(env, "exp", [](ArgVec args, CekCtx*) -> Value
      {
      if (is_complex(args[0])) { auto* c = as_complex(args[0]); auto r = std::exp(std::complex<double>(c->real, c->imag)); if (r.imag() == 0.0) return make_flonum(r.real()); return make_complex_val(r.real(), r.imag()); }
      try { return make_flonum(std::exp(scheme_to_double(args[0]))); } catch (...) { return make_flonum(std::numeric_limits<double>::infinity()); }
      }, 1, 1);

   def(env, "log", [](ArgVec args, CekCtx*) -> Value
      {
      double x = scheme_to_double(args[0]);
      double r;
      if (x == 0.0) r = -std::numeric_limits<double>::infinity();
      else if (x < 0.0) { std::complex<double> cr = std::log(std::complex<double>(x, 0.0)); if (args.size() >= 2) { double b = scheme_to_double(args[1]); cr /= std::complex<double>(std::log(b), 0.0); } if (cr.imag() == 0.0) return make_flonum(cr.real()); return make_complex_val(cr.real(), cr.imag()); }
      else r = std::log(x);
      if (args.size() >= 2) { double b = scheme_to_double(args[1]); if (b <= 0.0) r = std::numeric_limits<double>::quiet_NaN(); else r /= std::log(b); }
      return make_flonum(r);
      }, 1, 2);

   def(env, "sin",  [](ArgVec args, CekCtx*) -> Value { if (is_complex(args[0])) { auto* c = as_complex(args[0]); auto r = std::sin(std::complex<double>(c->real,c->imag)); return make_complex_val(r.real(),r.imag()); } return make_flonum(std::sin(scheme_to_double(args[0]))); }, 1, 1);
   def(env, "cos",  [](ArgVec args, CekCtx*) -> Value { if (is_complex(args[0])) { auto* c = as_complex(args[0]); auto r = std::cos(std::complex<double>(c->real,c->imag)); return make_complex_val(r.real(),r.imag()); } return make_flonum(std::cos(scheme_to_double(args[0]))); }, 1, 1);
   def(env, "tan",  [](ArgVec args, CekCtx*) -> Value { if (is_complex(args[0])) { auto* c = as_complex(args[0]); auto r = std::tan(std::complex<double>(c->real,c->imag)); return make_complex_val(r.real(),r.imag()); } return make_flonum(std::tan(scheme_to_double(args[0]))); }, 1, 1);
   def(env, "asin", [](ArgVec args, CekCtx*) -> Value { if (is_complex(args[0])) { auto* c = as_complex(args[0]); auto r = std::asin(std::complex<double>(c->real,c->imag)); return make_complex_val(r.real(),r.imag()); } double f = scheme_to_double(args[0]); if (f < -1.0 || f > 1.0) { auto r = std::asin(std::complex<double>(f,0.0)); return make_complex_val(r.real(),r.imag()); } return make_flonum(std::asin(f)); }, 1, 1);
   def(env, "acos", [](ArgVec args, CekCtx*) -> Value { if (is_complex(args[0])) { auto* c = as_complex(args[0]); auto r = std::acos(std::complex<double>(c->real,c->imag)); return make_complex_val(r.real(),r.imag()); } double f = scheme_to_double(args[0]); if (f < -1.0 || f > 1.0) { auto r = std::acos(std::complex<double>(f,0.0)); return make_complex_val(r.real(),r.imag()); } return make_flonum(std::acos(f)); }, 1, 1);
   def(env, "atan", [](ArgVec args, CekCtx*) -> Value
      {
      if (is_complex(args[0])) { auto* c = as_complex(args[0]); if (args.size() >= 2) throw SchemeError("atan: two-arg form requires real numbers"); auto r = std::atan(std::complex<double>(c->real,c->imag)); return make_complex_val(r.real(),r.imag()); }
      double f = scheme_to_double(args[0]);
      if (args.size() >= 2) return make_flonum(std::atan2(f, scheme_to_double(args[1])));
      return make_flonum(std::atan(f));
      }, 1, 2);

   def(env, "make-rectangular", [](ArgVec args, CekCtx*) -> Value
      {
      Value re = args[0], im = args[1];
      if (is_exact_num(re) && is_exact_num(im))
         {
         if (num_eq(im, make_fixnum(0))) return re;
         return make_exact_complex_val(gc_alloc_exact_complex(re, im));
         }
      return make_complex_val(scheme_to_double(re), scheme_to_double(im));
      }, 2, 2);

   def(env, "make-polar", [](ArgVec args, CekCtx*) -> Value
      {
      double r = scheme_to_double(args[0]), theta = scheme_to_double(args[1]);
      return make_complex_val(r * std::cos(theta), r * std::sin(theta));
      }, 2, 2);

   def(env, "real-part", [](ArgVec args, CekCtx*) -> Value
      {
      Value v = args[0];
      if (is_complex(v))        return make_flonum(as_complex(v)->real);
      if (is_exact_complex(v))  return as_exact_complex(v)->real;
      if (is_number(v))         return v;
      throw SchemeError("real-part: not a number");
      }, 1, 1);

   def(env, "imag-part", [](ArgVec args, CekCtx*) -> Value
      {
      Value v = args[0];
      if (is_complex(v))        return make_flonum(as_complex(v)->imag);
      if (is_exact_complex(v))  return as_exact_complex(v)->imag;
      if (is_fixnum(v) || is_rational(v))  return make_fixnum(0);
      if (is_flonum(v))         return make_flonum(0.0);
      throw SchemeError("imag-part: not a number");
      }, 1, 1);

   def(env, "magnitude", [](ArgVec args, CekCtx*) -> Value
      {
      Value v = args[0];
      if (is_complex(v))       { auto* c = as_complex(v); return make_flonum(std::hypot(c->real, c->imag)); }
      if (is_exact_complex(v)) { auto* ec = as_exact_complex(v); return make_flonum(std::hypot(scheme_to_double(ec->real), scheme_to_double(ec->imag))); }
      if (is_fixnum(v))  { int64_t n = as_fixnum(v); return make_fixnum(n < 0 ? -n : n); }
      if (is_rational(v)) { auto* r = as_rational(v); return make_rational(gc_alloc_rational(r->num < 0 ? -r->num : r->num, r->den)); }
      return make_flonum(std::abs(as_flonum(v)));
      }, 1, 1);

   def(env, "angle", [](ArgVec args, CekCtx*) -> Value
      {
      Value v = args[0];
      if (is_complex(v))       { auto* c = as_complex(v); return make_flonum(std::atan2(c->imag, c->real)); }
      if (is_exact_complex(v)) { auto* ec = as_exact_complex(v); return make_flonum(std::atan2(scheme_to_double(ec->imag), scheme_to_double(ec->real))); }
      double f = scheme_to_double(v);
      if (std::isnan(f)) return make_flonum(std::numeric_limits<double>::quiet_NaN());
      return make_flonum(f >= 0.0 ? 0.0 : M_PI);
      }, 1, 1);

   def(env, "rationalize", [](ArgVec args, CekCtx*) -> Value
      {
      double x     = scheme_to_double(args[0]);
      double delta = scheme_to_double(args[1]);
      bool inexact = is_flonum(args[0]) || is_flonum(args[1]);
      if (!std::isfinite(x)) return inexact ? make_flonum(x) : args[0];
      if (std::isinf(delta)) return inexact ? make_flonum(0.0) : make_fixnum(0);
      if (std::isnan(delta)) return make_flonum(std::numeric_limits<double>::quiet_NaN());
      if (delta < 0.0) delta = -delta;
      double lo = x - delta, hi = x + delta;
      if (lo <= 0.0 && hi >= 0.0) { return inexact ? make_flonum(0.0) : make_fixnum(0); }
      double sign = 1.0;
      if (lo < 0.0) { lo = -hi; hi = -lo + 2*delta; sign = -1.0; } // both negative
      auto [rn, rd] = simplest_rational_positive(lo, hi);
      int64_t num = (int64_t)(sign * rn), den = (int64_t)rd;
      if (!inexact)
         { if (den == 1) return make_fixnum(num); return make_rational(gc_alloc_rational(num, den)); }
      return make_flonum(sign * (double)rn / (double)rd);
      }, 2, 2);

   def(env, "number->string", [](ArgVec args, CekCtx*) -> Value
      {
      Value v = args[0];
      int radix = (args.size() >= 2) ? (int)as_fixnum(args[1]) : 10;
      std::function<std::string(int64_t)> fmt_int = [&](int64_t n) -> std::string
         {
         if (n < 0) { std::string s = "-"; s += fmt_int(-n); return s; }
         if (radix == 10) return std::to_string(n);
         const char* digs = "0123456789abcdef";
         if (n == 0) return "0";
         std::string s;
         while (n > 0) { s = digs[n % radix] + s; n /= radix; }
         return s;
         };
      if (is_fixnum(v))    return make_string(gc_alloc_string(fmt_int(as_fixnum(v))));
      if (is_rational(v))  { auto* r = as_rational(v); return make_string(gc_alloc_string(fmt_int(r->num) + "/" + fmt_int(r->den))); }
      if (radix != 10 && !is_fixnum(v)) throw SchemeError("number->string: radix != 10 only supported for exact integers");
      if (is_flonum(v))
         {
         double f = as_flonum(v);
         if (std::isnan(f)) return make_string(gc_alloc_string("+nan.0"));
         if (f == std::numeric_limits<double>::infinity()) return make_string(gc_alloc_string("+inf.0"));
         if (f == -std::numeric_limits<double>::infinity()) return make_string(gc_alloc_string("-inf.0"));
         std::ostringstream oss; oss << f;
         std::string s = oss.str();
         if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) s += ".0";
         return make_string(gc_alloc_string(s));
         }
      if (is_complex(v))
         {
         auto* c = as_complex(v);
         auto fmt_f = [](double f) -> std::string
            { if (std::isnan(f)) return "+nan.0"; if (f == std::numeric_limits<double>::infinity()) return "+inf.0"; if (f == -std::numeric_limits<double>::infinity()) return "-inf.0"; std::ostringstream oss; oss << f; std::string s = oss.str(); if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) s += ".0"; return s; };
         std::string re = fmt_f(c->real), im = fmt_f(c->imag);
         if (c->imag >= 0.0 || std::isnan(c->imag)) return make_string(gc_alloc_string(re + "+" + im + "i"));
         return make_string(gc_alloc_string(re + im + "i"));
         }
      throw SchemeError("number->string: not a number");
      }, 1, 2);

   def(env, "string->number", [](ArgVec args, CekCtx*) -> Value
      {
      if (!is_string(args[0])) throw SchemeError("string->number: first argument must be a string");
      std::string s = as_string(args[0])->data;
      // Trim leading/trailing whitespace
      while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
      while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
      int radix = (args.size() >= 2) ? (int)as_fixnum(args[1]) : 10;
      int exact = -1; // -1 unspecified, 0 inexact, 1 exact
      bool explicit_radix = false;
      // Strip prefix(es)
      while (s.size() >= 2 && s[0] == '#')
         {
         char c = (char)std::tolower(s[1]);
         if (c == 'b') { if (explicit_radix) return make_bool(false); radix = 2;  explicit_radix = true; s = s.substr(2); }
         else if (c == 'o') { if (explicit_radix) return make_bool(false); radix = 8;  explicit_radix = true; s = s.substr(2); }
         else if (c == 'd') { if (explicit_radix) return make_bool(false); radix = 10; explicit_radix = true; s = s.substr(2); }
         else if (c == 'x') { if (explicit_radix) return make_bool(false); radix = 16; explicit_radix = true; s = s.substr(2); }
         else if (c == 'e') { if (exact != -1) return make_bool(false); exact = 1; s = s.substr(2); }
         else if (c == 'i') { if (exact != -1) return make_bool(false); exact = 0; s = s.substr(2); }
         else return make_bool(false);
         }
      if (s.empty()) return make_bool(false);
      // Try integer parse
      try
         {
         size_t pos;
         long long n;
         if (radix == 10) n = std::stoll(s, &pos);
         else if (radix == 16) n = std::stoll(s, &pos, 16);
         else if (radix ==  8) n = std::stoll(s, &pos,  8);
         else if (radix ==  2) n = std::stoll(s, &pos,  2);
         else return make_bool(false);
         if (pos == s.size())
            {
            if (exact == 0) return make_flonum((double)n);
            return make_fixnum((int64_t)n);
            }
         }
      catch (...) {}
      // Try rational n/d
      {
      auto slash = s.find('/');
      if (slash != std::string::npos)
         {
         try
            {
            std::string num_s = s.substr(0, slash), den_s = s.substr(slash+1);
            int64_t num = (int64_t)std::stoll(num_s, nullptr, radix);
            int64_t den = (int64_t)std::stoll(den_s, nullptr, radix);
            if (den == 0) return make_bool(false);
            if (exact == 0) return make_flonum((double)num / (double)den);
            int64_t g = std::gcd(num < 0 ? -num : num, den); num /= g; den /= g;
            if (den == 1) return make_fixnum(num);
            if (den < 0)  { num = -num; den = -den; }
            return make_rational(gc_alloc_rational(num, den));
            }
         catch (...) {}
         }
      }
      if (radix == 10)
         {
         // Special flonum literals
         if (s == "+inf.0")  { if (exact == 1) return make_bool(false); return make_flonum(std::numeric_limits<double>::infinity()); }
         if (s == "-inf.0")  { if (exact == 1) return make_bool(false); return make_flonum(-std::numeric_limits<double>::infinity()); }
         if (s == "+nan.0" || s == "-nan.0") { if (exact == 1) return make_bool(false); return make_flonum(std::numeric_limits<double>::quiet_NaN()); }
         // Try complex via parser
         auto cv = parse_complex_literal(s);
         if (cv) { if (exact == 1) return make_bool(false); return *cv; }
         // Try float
         try
            {
            size_t pos;
            double f = std::stod(s, &pos);
            if (pos == s.size())
               {
               if (exact == 1) { if (!std::isfinite(f)) return make_bool(false); return flonum_to_exact(f); }
               return make_flonum(f);
               }
            }
         catch (...) {}
         }
      return make_bool(false);
      }, 1, 2);

   def(env, "random", [](ArgVec args, CekCtx*) -> Value
      {
      Value n = args[0];
      if (is_fixnum(n)) { int64_t v = as_fixnum(n); if (v <= 0) throw SchemeError("random: argument must be positive"); return make_fixnum((int64_t)rand() % v); }
      if (is_flonum(n)) { double v = as_flonum(n); if (v <= 0.0) throw SchemeError("random: argument must be positive"); return make_flonum(v * ((double)rand() / ((double)RAND_MAX + 1.0))); }
      throw SchemeError("random: argument must be a number");
      }, 1, 1);

   def(env, "features", [](ArgVec args, CekCtx*) -> Value
      {
      static const char* feats[] = {
         "r7rs", "cekscheme", "windows",
#ifdef _WIN32
         "win32",
#endif
         nullptr
      };
      Value result = make_nil();
      for (int i = 0; feats[i]; ++i)
         {
         auto* c = gc_alloc_cons(); c->car = make_symbol(feats[i]); c->cdr = result;
         result = make_cons(c);
         }
      return result;
      }, 0, 0);
   }

// ── Comparison section ────────────────────────────────────────────────────────
static void register_comparison(Environment* env)
   {
   auto cmp_chain = [](ArgVec args, auto cmp) -> Value
      {
      for (size_t i = 0; i + 1 < args.size(); ++i)
         if (!cmp(args[i], args[i+1])) return make_bool(false);
      return make_bool(true);
      };
   def(env, "=",  [cmp_chain](ArgVec args, CekCtx*){ return cmp_chain(args, num_eq); }, 1, -1);
   def(env, "<",  [cmp_chain](ArgVec args, CekCtx*){ return cmp_chain(args, num_lt); }, 1, -1);
   def(env, "<=", [cmp_chain](ArgVec args, CekCtx*){ return cmp_chain(args, num_le); }, 1, -1);
   def(env, ">",  [cmp_chain](ArgVec args, CekCtx*){ return cmp_chain(args, num_gt); }, 1, -1);
   def(env, ">=", [cmp_chain](ArgVec args, CekCtx*){ return cmp_chain(args, num_ge); }, 1, -1);
   }

// ── Type predicates section ───────────────────────────────────────────────────
static void register_predicates(Environment* env)
   {
   def(env, "null?",       [](ArgVec a, CekCtx*){ return make_bool(is_nil(a[0])); }, 1, 1);
   def(env, "pair?",       [](ArgVec a, CekCtx*){ return make_bool(is_cons(a[0])); }, 1, 1);
   def(env, "boolean?",    [](ArgVec a, CekCtx*){ return make_bool(is_bool(a[0])); }, 1, 1);
   def(env, "string?",     [](ArgVec a, CekCtx*){ return make_bool(is_string(a[0])); }, 1, 1);
   def(env, "symbol?",     [](ArgVec a, CekCtx*){ return make_bool(is_symbol(a[0])); }, 1, 1);
   def(env, "char?",       [](ArgVec a, CekCtx*){ return make_bool(is_char(a[0])); }, 1, 1);
   def(env, "vector?",     [](ArgVec a, CekCtx*){ return make_bool(is_vector(a[0])); }, 1, 1);
   def(env, "bytevector?", [](ArgVec a, CekCtx*){ return make_bool(is_bytevector(a[0])); }, 1, 1);
   def(env, "port?",       [](ArgVec a, CekCtx*){ return make_bool(is_port(a[0])); }, 1, 1);
   def(env, "input-port?", [](ArgVec a, CekCtx*){ return make_bool(is_input_port(a[0])); }, 1, 1);
   def(env, "output-port?",[](ArgVec a, CekCtx*){ return make_bool(is_output_port(a[0])); }, 1, 1);
   def(env, "eof-object?", [](ArgVec a, CekCtx*){ return make_bool(is_eof(a[0])); }, 1, 1);
   def(env, "procedure?",  [](ArgVec a, CekCtx*){ return make_bool(is_procedure(a[0])); }, 1, 1);
   def(env, "promise?",    [](ArgVec a, CekCtx*){ return make_bool(is_promise(a[0])); }, 1, 1);
   def(env, "parameter?",  [](ArgVec a, CekCtx*){ return make_bool(is_parameter(a[0])); }, 1, 1);
   def(env, "error-object?",[](ArgVec a, CekCtx*){ return make_bool(is_error_object(a[0])); }, 1, 1);

   def(env, "number?", [](ArgVec a, CekCtx*){ return make_bool(is_number(a[0]) || is_rational(a[0]) || is_complex(a[0]) || is_exact_complex(a[0])); }, 1, 1);
   def(env, "complex?", [](ArgVec a, CekCtx*){ return make_bool(is_number(a[0]) || is_rational(a[0]) || is_complex(a[0]) || is_exact_complex(a[0])); }, 1, 1);
   def(env, "real?", [](ArgVec a, CekCtx*) -> Value
      {
      Value v = a[0];
      if (is_fixnum(v) || is_flonum(v) || is_rational(v)) return make_bool(true);
      if (is_complex(v))       return make_bool(as_complex(v)->imag == 0.0);
      if (is_exact_complex(v)) { auto* ec = as_exact_complex(v); return make_bool(is_fixnum(ec->imag) && as_fixnum(ec->imag) == 0); }
      return make_bool(false);
      }, 1, 1);
   def(env, "rational?", [](ArgVec a, CekCtx*) -> Value
      {
      Value v = a[0];
      if (is_fixnum(v) || is_rational(v)) return make_bool(true);
      if (is_flonum(v)) return make_bool(std::isfinite(as_flonum(v)));
      if (is_complex(v)) return make_bool(as_complex(v)->imag == 0.0 && std::isfinite(as_complex(v)->real));
      if (is_exact_complex(v)) { auto* ec = as_exact_complex(v); return make_bool(is_fixnum(ec->imag) && as_fixnum(ec->imag) == 0); }
      return make_bool(false);
      }, 1, 1);
   def(env, "integer?", [](ArgVec a, CekCtx*) -> Value
      {
      Value v = a[0];
      if (is_fixnum(v)) return make_bool(true);
      if (is_flonum(v)) { double x = as_flonum(v); return make_bool(std::isfinite(x) && std::floor(x) == x); }
      if (is_complex(v)) { auto* c = as_complex(v); return make_bool(c->imag == 0.0 && std::isfinite(c->real) && std::floor(c->real) == c->real); }
      if (is_exact_complex(v)) { auto* ec = as_exact_complex(v); if (!(is_fixnum(ec->imag) && as_fixnum(ec->imag) == 0)) return make_bool(false); return make_bool(is_fixnum(ec->real)); }
      return make_bool(false);
      }, 1, 1);

   def(env, "exact-integer?", [](ArgVec a, CekCtx*){ return make_bool(is_fixnum(a[0])); }, 1, 1);

   def(env, "zero?",     [](ArgVec a, CekCtx*) -> Value
      { if (is_complex(a[0])) { auto* c = as_complex(a[0]); return make_bool(c->real == 0.0 && c->imag == 0.0); } return make_bool(num_eq(a[0], make_fixnum(0))); }, 1, 1);
   def(env, "positive?", [](ArgVec a, CekCtx*){ return make_bool(num_gt(a[0], make_fixnum(0))); }, 1, 1);
   def(env, "negative?", [](ArgVec a, CekCtx*){ return make_bool(num_lt(a[0], make_fixnum(0))); }, 1, 1);
   def(env, "odd?",  [](ArgVec a, CekCtx*) -> Value
      {
      if (is_fixnum(a[0])) return make_bool(as_fixnum(a[0]) % 2 != 0);
      if (is_flonum(a[0])) { double d = as_flonum(a[0]); return make_bool(std::fmod(d, 2.0) != 0.0); }
      throw SchemeError("odd?: not an integer");
      }, 1, 1);
   def(env, "even?", [](ArgVec a, CekCtx*) -> Value
      {
      if (is_fixnum(a[0])) return make_bool(as_fixnum(a[0]) % 2 == 0);
      if (is_flonum(a[0])) { double d = as_flonum(a[0]); return make_bool(std::fmod(d, 2.0) == 0.0); }
      throw SchemeError("even?: not an integer");
      }, 1, 1);

   def(env, "finite?", [](ArgVec a, CekCtx*) -> Value
      {
      Value v = a[0];
      if (is_fixnum(v) || is_rational(v) || is_exact_complex(v)) return make_bool(true);
      if (is_flonum(v)) return make_bool(std::isfinite(as_flonum(v)));
      if (is_complex(v)) { auto* c = as_complex(v); return make_bool(std::isfinite(c->real) && std::isfinite(c->imag)); }
      throw SchemeError("finite?: not a number");
      }, 1, 1);
   def(env, "infinite?", [](ArgVec a, CekCtx*) -> Value
      {
      Value v = a[0];
      if (is_fixnum(v) || is_rational(v) || is_exact_complex(v)) return make_bool(false);
      if (is_flonum(v)) return make_bool(std::isinf(as_flonum(v)));
      if (is_complex(v)) { auto* c = as_complex(v); return make_bool(std::isinf(c->real) || std::isinf(c->imag)); }
      throw SchemeError("infinite?: not a number");
      }, 1, 1);
   def(env, "nan?", [](ArgVec a, CekCtx*) -> Value
      {
      Value v = a[0];
      if (is_fixnum(v) || is_rational(v) || is_exact_complex(v)) return make_bool(false);
      if (is_flonum(v)) return make_bool(std::isnan(as_flonum(v)));
      if (is_complex(v)) { auto* c = as_complex(v); return make_bool(std::isnan(c->real) || std::isnan(c->imag)); }
      throw SchemeError("nan?: not a number");
      }, 1, 1);

   def(env, "list?", [](ArgVec a, CekCtx*) -> Value
      {
      Value slow = a[0], fast = a[0];
      while (true)
         {
         if (is_nil(fast)) return make_bool(true);
         if (!is_cons(fast)) return make_bool(false);
         fast = cdr(fast);
         if (is_nil(fast)) return make_bool(true);
         if (!is_cons(fast)) return make_bool(false);
         fast = cdr(fast);
         slow = cdr(slow);
         if (values_eq(fast, slow)) return make_bool(false);
         }
      }, 1, 1);

   def(env, "not", [](ArgVec a, CekCtx*){ return make_bool(!is_truthy(a[0])); }, 1, 1);

   def(env, "input-port-open?",  [](ArgVec a, CekCtx*) -> Value { if (!is_port(a[0])) return make_bool(false); return make_bool(as_port(a[0])->is_open && as_port(a[0])->is_input); }, 1, 1);
   def(env, "output-port-open?", [](ArgVec a, CekCtx*) -> Value { if (!is_port(a[0])) return make_bool(false); return make_bool(as_port(a[0])->is_open && as_port(a[0])->is_output); }, 1, 1);
   def(env, "textual-port?", [](ArgVec a, CekCtx*) -> Value { if (!is_port(a[0])) return make_bool(false); return make_bool(as_port(a[0])->is_text); }, 1, 1);
   def(env, "binary-port?",  [](ArgVec a, CekCtx*) -> Value { if (!is_port(a[0])) return make_bool(false); return make_bool(!as_port(a[0])->is_text); }, 1, 1);
   }

// ── Equality section ──────────────────────────────────────────────────────────
static void register_equality(Environment* env)
   {
   def(env, "eq?",    [](ArgVec a, CekCtx*){ return make_bool(values_eq(a[0], a[1])); }, 2, 2);
   def(env, "eqv?",   [](ArgVec a, CekCtx*){ return make_bool(values_eqv(a[0], a[1])); }, 2, 2);
   def(env, "equal?", [](ArgVec a, CekCtx*){ return make_bool(values_equal(a[0], a[1])); }, 2, 2);

   def(env, "boolean=?", [](ArgVec args, CekCtx*) -> Value
      {
      if (!is_bool(args[0])) throw SchemeTypeError("boolean=?: arguments must be booleans");
      bool first = as_bool(args[0]);
      for (size_t i = 1; i < args.size(); ++i)
         { if (!is_bool(args[i])) throw SchemeTypeError("boolean=?: arguments must be booleans"); if (as_bool(args[i]) != first) return make_bool(false); }
      return make_bool(true);
      }, 2, -1);

   def(env, "symbol=?", [](ArgVec args, CekCtx*) -> Value
      {
      if (!is_symbol(args[0])) throw SchemeTypeError("symbol=?: arguments must be symbols");
      uint32_t first = as_symbol_id(args[0]);
      for (size_t i = 1; i < args.size(); ++i)
         { if (!is_symbol(args[i])) throw SchemeTypeError("symbol=?: arguments must be symbols"); if (as_symbol_id(args[i]) != first) return make_bool(false); }
      return make_bool(true);
      }, 2, -1);
   }

// ── Pairs and lists ───────────────────────────────────────────────────────────
static void register_pairs(Environment* env)
   {
   def(env, "cons", [](ArgVec args, CekCtx*) -> Value
      { auto* c = gc_alloc_cons(); c->car = args[0]; c->cdr = args[1]; return make_cons(c); }, 2, 2);
   def(env, "car", [](ArgVec args, CekCtx*){ return car(args[0]); }, 1, 1);
   def(env, "cdr", [](ArgVec args, CekCtx*){ return cdr(args[0]); }, 1, 1);
   def(env, "set-car!", [](ArgVec args, CekCtx*){ set_car(args[0], args[1]); return make_unspecified(); }, 2, 2);
   def(env, "set-cdr!", [](ArgVec args, CekCtx*){ set_cdr(args[0], args[1]); return make_unspecified(); }, 2, 2);

   // cXXr family
   auto make_cxr = [](const char* name, std::string ops) -> BuiltinFn
      {
      return [name, ops](ArgVec args, CekCtx*) -> Value
         {
         Value v = args[0];
         for (int i = (int)ops.size() - 1; i >= 0; --i)
            {
            if (!is_cons(v)) throw SchemeTypeError(std::string(name) + ": not a pair");
            v = (ops[i] == 'a') ? car(v) : cdr(v);
            }
         return v;
         };
      };

   def(env, "caar",   make_cxr("caar",   "aa"), 1, 1);
   def(env, "cadr",   make_cxr("cadr",   "ad"), 1, 1);
   def(env, "cdar",   make_cxr("cdar",   "da"), 1, 1);
   def(env, "cddr",   make_cxr("cddr",   "dd"), 1, 1);
   def(env, "caaar",  make_cxr("caaar",  "aaa"), 1, 1);
   def(env, "caadr",  make_cxr("caadr",  "aad"), 1, 1);
   def(env, "cadar",  make_cxr("cadar",  "ada"), 1, 1);
   def(env, "caddr",  make_cxr("caddr",  "add"), 1, 1);
   def(env, "cdaar",  make_cxr("cdaar",  "daa"), 1, 1);
   def(env, "cdadr",  make_cxr("cdadr",  "dad"), 1, 1);
   def(env, "cddar",  make_cxr("cddar",  "dda"), 1, 1);
   def(env, "cdddr",  make_cxr("cdddr",  "ddd"), 1, 1);
   def(env, "caaaar", make_cxr("caaaar", "aaaa"), 1, 1);
   def(env, "caaadr", make_cxr("caaadr", "aaad"), 1, 1);
   def(env, "caadar", make_cxr("caadar", "aada"), 1, 1);
   def(env, "caaddr", make_cxr("caaddr", "aadd"), 1, 1);
   def(env, "cadaar", make_cxr("cadaar", "adaa"), 1, 1);
   def(env, "cadadr", make_cxr("cadadr", "adad"), 1, 1);
   def(env, "caddar", make_cxr("caddar", "adda"), 1, 1);
   def(env, "cadddr", make_cxr("cadddr", "addd"), 1, 1);
   def(env, "cdaaar", make_cxr("cdaaar", "daaa"), 1, 1);
   def(env, "cdaadr", make_cxr("cdaadr", "daad"), 1, 1);
   def(env, "cdadar", make_cxr("cdadar", "dada"), 1, 1);
   def(env, "cdaddr", make_cxr("cdaddr", "dadd"), 1, 1);
   def(env, "cddaar", make_cxr("cddaar", "ddaa"), 1, 1);
   def(env, "cddadr", make_cxr("cddadr", "ddad"), 1, 1);
   def(env, "cdddar", make_cxr("cdddar", "ddda"), 1, 1);
   def(env, "cddddr", make_cxr("cddddr", "dddd"), 1, 1);

   def(env, "list", [](ArgVec args, CekCtx*) -> Value
      {
      Value r = make_nil();
      for (int i = (int)args.size()-1; i >= 0; --i) { auto* c = gc_alloc_cons(); c->car = args[i]; c->cdr = r; r = make_cons(c); }
      return r;
      }, 0, -1);

   def(env, "list*", [](ArgVec args, CekCtx*) -> Value
      {
      Value r = args.back();
      for (int i = (int)args.size()-2; i >= 0; --i) { auto* c = gc_alloc_cons(); c->car = args[i]; c->cdr = r; r = make_cons(c); }
      return r;
      }, 1, -1);

   def(env, "length", [](ArgVec args, CekCtx*) -> Value
      {
      int64_t n = 0; Value v = args[0];
      while (is_cons(v)) { ++n; v = cdr(v); }
      if (!is_nil(v)) throw SchemeTypeError("length: not a proper list");
      return make_fixnum(n);
      }, 1, 1);

   def(env, "append", [](ArgVec args, CekCtx*) -> Value
      {
      if (args.empty()) return make_nil();
      if (args.size() == 1) return args[0];
      std::vector<Value> items;
      for (size_t i = 0; i + 1 < args.size(); ++i)
         {
         Value v = args[i];
         while (is_cons(v)) { items.push_back(car(v)); v = cdr(v); }
         if (!is_nil(v)) throw SchemeTypeError("append: non-last argument must be a proper list");
         }
      Value r = args.back();
      for (int i = (int)items.size()-1; i >= 0; --i) { auto* c = gc_alloc_cons(); c->car = items[i]; c->cdr = r; r = make_cons(c); }
      return r;
      }, 0, -1);

   def(env, "reverse", [](ArgVec args, CekCtx*) -> Value
      {
      Value r = make_nil(); Value v = args[0];
      while (is_cons(v)) { auto* c = gc_alloc_cons(); c->car = car(v); c->cdr = r; r = make_cons(c); v = cdr(v); }
      if (!is_nil(v)) throw SchemeTypeError("reverse: not a proper list");
      return r;
      }, 1, 1);

   def(env, "list-tail", [](ArgVec args, CekCtx*) -> Value
      {
      Value v = args[0]; int64_t k = as_fixnum(args[1]);
      while (k-- > 0) { if (!is_cons(v)) throw SchemeError("list-tail: index out of range"); v = cdr(v); }
      return v;
      }, 2, 2);

   def(env, "list-ref", [](ArgVec args, CekCtx*) -> Value
      {
      Value v = args[0]; int64_t k = as_fixnum(args[1]);
      while (k-- > 0) { if (!is_cons(v)) throw SchemeError("list-ref: index out of range"); v = cdr(v); }
      return car(v);
      }, 2, 2);

   def(env, "list-set!", [](ArgVec args, CekCtx*) -> Value
      {
      Value v = args[0]; int64_t k = as_fixnum(args[1]);
      while (k-- > 0) { if (!is_cons(v)) throw SchemeError("list-set!: index out of range"); v = cdr(v); }
      if (!is_cons(v)) throw SchemeError("list-set!: index out of range");
      set_car(v, args[2]);
      return make_unspecified();
      }, 3, 3);

   def(env, "list-copy", [](ArgVec args, CekCtx*) -> Value
      {
      Value v = args[0];
      if (!is_cons(v)) return v;
      std::vector<Value> items;
      while (is_cons(v)) { items.push_back(car(v)); v = cdr(v); }
      Value r = v;
      for (int i = (int)items.size()-1; i >= 0; --i) { auto* c = gc_alloc_cons(); c->car = items[i]; c->cdr = r; r = make_cons(c); }
      return r;
      }, 1, 1);

   def(env, "make-list", [](ArgVec args, CekCtx*) -> Value
      {
      int64_t k = as_fixnum(args[0]);
      if (k < 0) throw SchemeError("make-list: negative length");
      Value fill = (args.size() >= 2) ? args[1] : make_bool(false);
      Value r = make_nil();
      while (k-- > 0) { auto* c = gc_alloc_cons(); c->car = fill; c->cdr = r; r = make_cons(c); }
      return r;
      }, 1, 2);

   def(env, "member", [](ArgVec args, CekCtx* ctx) -> Value
      {
      Value key = args[0], lst = args[1];
      if (args.size() >= 3)
         {
         Value cmp = args[2];
         while (is_cons(lst)) { if (is_truthy(apply_scheme_proc(cmp, {key, car(lst)}, *ctx))) return lst; lst = cdr(lst); }
         return make_bool(false);
         }
      while (is_cons(lst)) { if (values_equal(car(lst), key)) return lst; lst = cdr(lst); }
      return make_bool(false);
      }, 2, 3);

   def(env, "memv", [](ArgVec args, CekCtx*) -> Value
      { Value key = args[0], lst = args[1]; while (is_cons(lst)) { if (values_eqv(car(lst), key)) return lst; lst = cdr(lst); } return make_bool(false); }, 2, 2);

   def(env, "memq", [](ArgVec args, CekCtx*) -> Value
      { Value key = args[0], lst = args[1]; while (is_cons(lst)) { if (values_eq(car(lst), key)) return lst; lst = cdr(lst); } return make_bool(false); }, 2, 2);

   def(env, "assoc", [](ArgVec args, CekCtx* ctx) -> Value
      {
      Value key = args[0], lst = args[1];
      if (args.size() >= 3)
         {
         Value cmp = args[2];
         while (is_cons(lst)) { Value pair = car(lst); if (is_cons(pair) && is_truthy(apply_scheme_proc(cmp, {key, car(pair)}, *ctx))) return pair; lst = cdr(lst); }
         return make_bool(false);
         }
      while (is_cons(lst)) { Value pair = car(lst); if (is_cons(pair) && values_equal(car(pair), key)) return pair; lst = cdr(lst); }
      return make_bool(false);
      }, 2, 3);

   def(env, "assv", [](ArgVec args, CekCtx*) -> Value
      { Value key = args[0], lst = args[1]; while (is_cons(lst)) { Value pair = car(lst); if (is_cons(pair) && values_eqv(car(pair), key)) return pair; lst = cdr(lst); } return make_bool(false); }, 2, 2);

   def(env, "assq", [](ArgVec args, CekCtx*) -> Value
      { Value key = args[0], lst = args[1]; while (is_cons(lst)) { Value pair = car(lst); if (is_cons(pair) && values_eq(car(pair), key)) return pair; lst = cdr(lst); } return make_bool(false); }, 2, 2);

   def(env, "map", [](ArgVec args, CekCtx* ctx) -> Value
      {
      Value proc = args[0];
      std::vector<Value> lists(args.begin()+1, args.end());
      std::vector<Value> collected;
      while (true)
         {
         bool done = false;
         for (auto& l : lists) if (!is_cons(l)) { done = true; break; }
         if (done) break;
         std::vector<Value> row;
         for (auto& l : lists) row.push_back(car(l));
         collected.push_back(apply_scheme_proc(proc, row, *ctx));
         for (auto& l : lists) l = cdr(l);
         }
      Value r = make_nil();
      for (int i = (int)collected.size()-1; i >= 0; --i) { auto* c = gc_alloc_cons(); c->car = collected[i]; c->cdr = r; r = make_cons(c); }
      return r;
      }, 2, -1);

   def(env, "for-each", [](ArgVec args, CekCtx* ctx) -> Value
      {
      Value proc = args[0];
      std::vector<Value> lists(args.begin()+1, args.end());
      while (true)
         {
         bool done = false;
         for (auto& l : lists) if (!is_cons(l)) { done = true; break; }
         if (done) break;
         std::vector<Value> row;
         for (auto& l : lists) row.push_back(car(l));
         apply_scheme_proc(proc, row, *ctx);
         for (auto& l : lists) l = cdr(l);
         }
      return make_unspecified();
      }, 2, -1);

   // filter / fold (common SRFI-1 extensions)
   def(env, "filter", [](ArgVec args, CekCtx* ctx) -> Value
      {
      Value pred = args[0], lst = args[1];
      std::vector<Value> items;
      while (is_cons(lst)) { if (is_truthy(apply_scheme_proc(pred, {car(lst)}, *ctx))) items.push_back(car(lst)); lst = cdr(lst); }
      Value r = make_nil();
      for (int i = (int)items.size()-1; i >= 0; --i) { auto* c = gc_alloc_cons(); c->car = items[i]; c->cdr = r; r = make_cons(c); }
      return r;
      }, 2, 2);

   def(env, "fold-left", [](ArgVec args, CekCtx* ctx) -> Value
      {
      Value proc = args[0], init = args[1], lst = args[2];
      Value acc = init;
      while (is_cons(lst)) { acc = apply_scheme_proc(proc, {acc, car(lst)}, *ctx); lst = cdr(lst); }
      return acc;
      }, 3, 3);

   def(env, "fold-right", [](ArgVec args, CekCtx* ctx) -> Value
      {
      Value proc = args[0], init = args[1], lst = args[2];
      std::vector<Value> items;
      while (is_cons(lst)) { items.push_back(car(lst)); lst = cdr(lst); }
      Value acc = init;
      for (int i = (int)items.size()-1; i >= 0; --i) acc = apply_scheme_proc(proc, {items[i], acc}, *ctx);
      return acc;
      }, 3, 3);

   def(env, "iota", [](ArgVec args, CekCtx*) -> Value
      {
      int64_t count = as_fixnum(args[0]);
      if (count < 0) throw SchemeError("iota: negative count");
      Value start = (args.size() >= 2) ? args[1] : make_fixnum(0);
      Value step  = (args.size() >= 3) ? args[2] : make_fixnum(1);
      Value r = make_nil();
      for (int64_t i = count - 1; i >= 0; --i)
         {
         Value idx = make_fixnum(i);
         Value elem = num_add(start, num_mul(step, idx));
         auto* c = gc_alloc_cons(); c->car = elem; c->cdr = r; r = make_cons(c);
         }
      return r;
      }, 1, 3);
   }

// ── Strings ───────────────────────────────────────────────────────────────────
static void register_strings(Environment* env)
   {
   def(env, "string", [](ArgVec args, CekCtx*) -> Value
      {
      std::string s;
      for (auto& a : args) s += codepoint_to_utf8(as_char(a));
      return make_string(gc_alloc_string(s));
      }, 0, -1);

   def(env, "make-string", [](ArgVec args, CekCtx*) -> Value
      {
      int64_t k = as_fixnum(args[0]);
      if (k < 0) throw SchemeError("make-string: negative length");
      char32_t fill = (args.size() >= 2) ? as_char(args[1]) : U' ';
      std::string s;
      for (int64_t i = 0; i < k; ++i) s += codepoint_to_utf8(fill);
      return make_string(gc_alloc_string(s));
      }, 1, 2);

   def(env, "string-length", [](ArgVec args, CekCtx*) -> Value
      { return make_fixnum((int64_t)utf8_codepoint_count(as_string(args[0])->data)); }, 1, 1);

   def(env, "string-ref", [](ArgVec args, CekCtx*) -> Value
      {
      const std::string& s = as_string(args[0])->data;
      int64_t k = as_fixnum(args[1]);
      size_t n = utf8_codepoint_count(s);
      if (k < 0 || (size_t)k >= n) throw SchemeError("string-ref: index out of range");
      return make_char(utf8_codepoint_at(s, (size_t)k));
      }, 2, 2);

   def(env, "string-set!", [](ArgVec args, CekCtx*) -> Value
      {
      SchemeString* ss = as_string(args[0]);
      int64_t k = as_fixnum(args[1]);
      char32_t cp = as_char(args[2]);
      size_t n = utf8_codepoint_count(ss->data);
      if (k < 0 || (size_t)k >= n) throw SchemeError("string-set!: index out of range");
      ss->data = utf8_set_at(ss->data, (size_t)k, cp);
      return make_unspecified();
      }, 3, 3);

   def(env, "string-fill!", [](ArgVec args, CekCtx*) -> Value
      {
      SchemeString* ss = as_string(args[0]);
      char32_t cp = as_char(args[1]);
      size_t n = utf8_codepoint_count(ss->data);
      size_t start = (args.size() >= 3) ? (size_t)as_fixnum(args[2]) : 0;
      size_t end   = (args.size() >= 4) ? (size_t)as_fixnum(args[3]) : n;
      std::string r;
      size_t pos = 0, idx = 0;
      while (pos < ss->data.size())
         {
         size_t s0 = pos;
         char32_t c = utf8_decode_one(ss->data, pos);
         if (idx >= start && idx < end) r += codepoint_to_utf8(cp);
         else                            r += ss->data.substr(s0, pos - s0);
         ++idx;
         }
      ss->data = r;
      return make_unspecified();
      }, 2, 4);

   def(env, "substring", [](ArgVec args, CekCtx*) -> Value
      {
      const std::string& s = as_string(args[0])->data;
      int64_t start = as_fixnum(args[1]);
      size_t n = utf8_codepoint_count(s);
      size_t end = (args.size() >= 3) ? (size_t)as_fixnum(args[2]) : n;
      if (start < 0 || (size_t)start > end || end > n) throw SchemeError("substring: index out of range");
      std::string r;
      size_t pos = 0;
      for (size_t i = 0; i < end; ++i)
         {
         size_t s0 = pos;
         char32_t cp = utf8_decode_one(s, pos);
         if (i >= (size_t)start) r += codepoint_to_utf8(cp);
         (void)cp;
         }
      return make_string(gc_alloc_string(r));
      }, 2, 3);

   def(env, "string-append", [](ArgVec args, CekCtx*) -> Value
      {
      std::string r;
      for (auto& a : args) r += as_string(a)->data;
      return make_string(gc_alloc_string(r));
      }, 0, -1);

   def(env, "string-copy", [](ArgVec args, CekCtx*) -> Value
      {
      const std::string& s = as_string(args[0])->data;
      size_t n = utf8_codepoint_count(s);
      size_t start = (args.size() >= 2) ? (size_t)as_fixnum(args[1]) : 0;
      size_t end   = (args.size() >= 3) ? (size_t)as_fixnum(args[2]) : n;
      if (start > end || end > n) throw SchemeError("string-copy: index out of range");
      std::string r;
      size_t pos = 0;
      for (size_t i = 0; i < end; ++i)
         {
         size_t s0 = pos;
         char32_t cp = utf8_decode_one(s, pos);
         if (i >= start) r += codepoint_to_utf8(cp);
         (void)cp;
         }
      return make_string(gc_alloc_string(r));
      }, 1, 3);

   def(env, "string-copy!", [](ArgVec args, CekCtx*) -> Value
      {
      SchemeString* to = as_string(args[0]);
      int64_t at = as_fixnum(args[1]);
      const std::string& from = as_string(args[2])->data;
      size_t from_n = utf8_codepoint_count(from);
      size_t start = (args.size() >= 4) ? (size_t)as_fixnum(args[3]) : 0;
      size_t end   = (args.size() >= 5) ? (size_t)as_fixnum(args[4]) : from_n;
      // Build sub of from[start..end)
      std::string sub;
      size_t pos = 0;
      for (size_t i = 0; i < end; ++i)
         {
         size_t s0 = pos;
         char32_t cp = utf8_decode_one(from, pos);
         if (i >= start) sub += codepoint_to_utf8(cp);
         (void)cp;
         }
      // Apply to to starting at at
      std::string result;
      size_t to_n = utf8_codepoint_count(to->data);
      size_t to_pos = 0;
      size_t sub_pos = 0;
      for (size_t i = 0; i < to_n; ++i)
         {
         size_t s0 = to_pos;
         char32_t cp = utf8_decode_one(to->data, to_pos);
         if (i >= (size_t)at && sub_pos < sub.size())
            {
            char32_t sc = utf8_decode_one(sub, sub_pos);
            result += codepoint_to_utf8(sc);
            }
         else
            result += to->data.substr(s0, to_pos - s0);
         (void)cp;
         }
      to->data = result;
      return make_unspecified();
      }, 3, 5);

   def(env, "string->list", [](ArgVec args, CekCtx*) -> Value
      {
      const std::string& s = as_string(args[0])->data;
      size_t n = utf8_codepoint_count(s);
      size_t start = (args.size() >= 2) ? (size_t)as_fixnum(args[1]) : 0;
      size_t end   = (args.size() >= 3) ? (size_t)as_fixnum(args[2]) : n;
      std::vector<char32_t> cps;
      size_t pos = 0;
      for (size_t i = 0; i < n; ++i) { char32_t cp = utf8_decode_one(s, pos); if (i >= start && i < end) cps.push_back(cp); }
      Value r = make_nil();
      for (int i = (int)cps.size()-1; i >= 0; --i) { auto* c = gc_alloc_cons(); c->car = make_char(cps[i]); c->cdr = r; r = make_cons(c); }
      return r;
      }, 1, 3);

   def(env, "list->string", [](ArgVec args, CekCtx*) -> Value
      {
      std::string s;
      Value lst = args[0];
      while (is_cons(lst)) { s += codepoint_to_utf8(as_char(car(lst))); lst = cdr(lst); }
      return make_string(gc_alloc_string(s));
      }, 1, 1);

   def(env, "string->symbol", [](ArgVec args, CekCtx*) -> Value
      { return make_symbol(as_string(args[0])->data); }, 1, 1);

   def(env, "symbol->string", [](ArgVec args, CekCtx*) -> Value
      {
      if (!is_symbol(args[0])) throw SchemeTypeError("symbol->string: not a symbol");
      return make_string(gc_alloc_string(symbol_name(as_symbol_id(args[0]))));
      }, 1, 1);

   def(env, "string-upcase", [](ArgVec args, CekCtx*) -> Value
      {
      std::string s; const std::string& src = as_string(args[0])->data;
      size_t pos = 0;
      while (pos < src.size()) { char32_t cp = utf8_decode_one(src, pos); if (cp < 128) cp = (char32_t)std::toupper((int)cp); s += codepoint_to_utf8(cp); }
      return make_string(gc_alloc_string(s));
      }, 1, 1);

   def(env, "string-downcase", [](ArgVec args, CekCtx*) -> Value
      {
      std::string s; const std::string& src = as_string(args[0])->data;
      size_t pos = 0;
      while (pos < src.size()) { char32_t cp = utf8_decode_one(src, pos); if (cp < 128) cp = (char32_t)std::tolower((int)cp); s += codepoint_to_utf8(cp); }
      return make_string(gc_alloc_string(s));
      }, 1, 1);

   def(env, "string-foldcase", [](ArgVec args, CekCtx*) -> Value
      {
      std::string s; const std::string& src = as_string(args[0])->data;
      size_t pos = 0;
      while (pos < src.size()) { char32_t cp = utf8_decode_one(src, pos); if (cp < 128) cp = (char32_t)std::tolower((int)cp); s += codepoint_to_utf8(cp); }
      return make_string(gc_alloc_string(s));
      }, 1, 1);

   // String comparisons (operate on UTF-8 code-unit order for ASCII strings;
   // for full Unicode order, see string-ci=? etc. using tolower)
   auto str_cmp = [](ArgVec args, CekCtx*, auto op) -> Value
      {
      for (size_t i = 0; i + 1 < args.size(); ++i)
         if (!op(as_string(args[i])->data, as_string(args[i+1])->data)) return make_bool(false);
      return make_bool(true);
      };

   def(env, "string=?",  [str_cmp](ArgVec a, CekCtx* c){ return str_cmp(a, c, [](const std::string& x, const std::string& y){ return x == y; }); }, 2, -1);
   def(env, "string<?",  [str_cmp](ArgVec a, CekCtx* c){ return str_cmp(a, c, [](const std::string& x, const std::string& y){ return x < y; }); }, 2, -1);
   def(env, "string<=?", [str_cmp](ArgVec a, CekCtx* c){ return str_cmp(a, c, [](const std::string& x, const std::string& y){ return x <= y; }); }, 2, -1);
   def(env, "string>?",  [str_cmp](ArgVec a, CekCtx* c){ return str_cmp(a, c, [](const std::string& x, const std::string& y){ return x > y; }); }, 2, -1);
   def(env, "string>=?", [str_cmp](ArgVec a, CekCtx* c){ return str_cmp(a, c, [](const std::string& x, const std::string& y){ return x >= y; }); }, 2, -1);

   auto str_ci_cmp = [](ArgVec args, CekCtx*, auto op) -> Value
      {
      auto to_lower = [](const std::string& s) { std::string r = s; for (char& c : r) if ((unsigned char)c < 128) c = (char)std::tolower((unsigned char)c); return r; };
      for (size_t i = 0; i + 1 < args.size(); ++i)
         if (!op(to_lower(as_string(args[i])->data), to_lower(as_string(args[i+1])->data))) return make_bool(false);
      return make_bool(true);
      };

   def(env, "string-ci=?",  [str_ci_cmp](ArgVec a, CekCtx* c){ return str_ci_cmp(a, c, [](const std::string& x, const std::string& y){ return x == y; }); }, 2, -1);
   def(env, "string-ci<?",  [str_ci_cmp](ArgVec a, CekCtx* c){ return str_ci_cmp(a, c, [](const std::string& x, const std::string& y){ return x < y; }); }, 2, -1);
   def(env, "string-ci<=?", [str_ci_cmp](ArgVec a, CekCtx* c){ return str_ci_cmp(a, c, [](const std::string& x, const std::string& y){ return x <= y; }); }, 2, -1);
   def(env, "string-ci>?",  [str_ci_cmp](ArgVec a, CekCtx* c){ return str_ci_cmp(a, c, [](const std::string& x, const std::string& y){ return x > y; }); }, 2, -1);
   def(env, "string-ci>=?", [str_ci_cmp](ArgVec a, CekCtx* c){ return str_ci_cmp(a, c, [](const std::string& x, const std::string& y){ return x >= y; }); }, 2, -1);

   def(env, "string-for-each", [](ArgVec args, CekCtx* ctx) -> Value
      {
      Value proc = args[0];
      std::vector<std::string> strs;
      for (size_t i = 1; i < args.size(); ++i) strs.push_back(as_string(args[i])->data);
      if (strs.empty()) return make_unspecified();
      std::vector<size_t> positions(strs.size(), 0);
      while (true)
         {
         bool done = false;
         for (size_t j = 0; j < strs.size(); ++j) if (positions[j] >= strs[j].size()) { done = true; break; }
         if (done) break;
         std::vector<Value> row;
         for (size_t j = 0; j < strs.size(); ++j) row.push_back(make_char(utf8_decode_one(strs[j], positions[j])));
         apply_scheme_proc(proc, row, *ctx);
         }
      return make_unspecified();
      }, 2, -1);

   def(env, "string-map", [](ArgVec args, CekCtx* ctx) -> Value
      {
      Value proc = args[0];
      std::vector<std::string> strs;
      for (size_t i = 1; i < args.size(); ++i) strs.push_back(as_string(args[i])->data);
      if (strs.empty()) return make_string(gc_alloc_string(""));
      std::vector<size_t> positions(strs.size(), 0);
      std::string result;
      while (true)
         {
         bool done = false;
         for (size_t j = 0; j < strs.size(); ++j) if (positions[j] >= strs[j].size()) { done = true; break; }
         if (done) break;
         std::vector<Value> row;
         for (size_t j = 0; j < strs.size(); ++j) row.push_back(make_char(utf8_decode_one(strs[j], positions[j])));
         Value r = apply_scheme_proc(proc, row, *ctx);
         result += codepoint_to_utf8(as_char(r));
         }
      return make_string(gc_alloc_string(result));
      }, 2, -1);

   }

// ── Characters ────────────────────────────────────────────────────────────────
static void register_chars(Environment* env)
   {
   def(env, "char->integer",   [](ArgVec a, CekCtx*){ return make_fixnum((int64_t)as_char(a[0])); }, 1, 1);
   def(env, "integer->char",   [](ArgVec a, CekCtx*){ int64_t n = as_fixnum(a[0]); if (n < 0) throw SchemeError("integer->char: negative"); return make_char((char32_t)n); }, 1, 1);
   def(env, "char-alphabetic?",[](ArgVec a, CekCtx*){ char32_t c = as_char(a[0]); return make_bool(c < 128 && std::isalpha((int)c)); }, 1, 1);
   def(env, "char-numeric?",   [](ArgVec a, CekCtx*){ char32_t c = as_char(a[0]); return make_bool(c < 128 && std::isdigit((int)c)); }, 1, 1);
   def(env, "char-whitespace?",[](ArgVec a, CekCtx*){ char32_t c = as_char(a[0]); return make_bool(c < 128 && std::isspace((int)c)); }, 1, 1);
   def(env, "char-upper-case?",[](ArgVec a, CekCtx*){ char32_t c = as_char(a[0]); return make_bool(c < 128 && std::isupper((int)c)); }, 1, 1);
   def(env, "char-lower-case?",[](ArgVec a, CekCtx*){ char32_t c = as_char(a[0]); return make_bool(c < 128 && std::islower((int)c)); }, 1, 1);
   def(env, "char-upcase",     [](ArgVec a, CekCtx*){ char32_t c = as_char(a[0]); return make_char(c < 128 ? (char32_t)std::toupper((int)c) : c); }, 1, 1);
   def(env, "char-downcase",   [](ArgVec a, CekCtx*){ char32_t c = as_char(a[0]); return make_char(c < 128 ? (char32_t)std::tolower((int)c) : c); }, 1, 1);
   def(env, "char-foldcase",   [](ArgVec a, CekCtx*){ char32_t c = as_char(a[0]); return make_char(c < 128 ? (char32_t)std::tolower((int)c) : c); }, 1, 1);

   def(env, "digit-value", [](ArgVec a, CekCtx*) -> Value
      {
      char32_t c = as_char(a[0]);
      if (c >= U'0' && c <= U'9') return make_fixnum((int64_t)(c - U'0'));
      return make_bool(false);
      }, 1, 1);

   auto char_cmp = [](ArgVec args, CekCtx*, auto op) -> Value
      {
      for (size_t i = 0; i + 1 < args.size(); ++i)
         if (!op(as_char(args[i]), as_char(args[i+1]))) return make_bool(false);
      return make_bool(true);
      };
   def(env, "char=?",  [char_cmp](ArgVec a, CekCtx* c){ return char_cmp(a, c, [](char32_t x, char32_t y){ return x == y; }); }, 2, -1);
   def(env, "char<?",  [char_cmp](ArgVec a, CekCtx* c){ return char_cmp(a, c, [](char32_t x, char32_t y){ return x < y; }); }, 2, -1);
   def(env, "char<=?", [char_cmp](ArgVec a, CekCtx* c){ return char_cmp(a, c, [](char32_t x, char32_t y){ return x <= y; }); }, 2, -1);
   def(env, "char>?",  [char_cmp](ArgVec a, CekCtx* c){ return char_cmp(a, c, [](char32_t x, char32_t y){ return x > y; }); }, 2, -1);
   def(env, "char>=?", [char_cmp](ArgVec a, CekCtx* c){ return char_cmp(a, c, [](char32_t x, char32_t y){ return x >= y; }); }, 2, -1);

   auto char_ci_cmp = [](ArgVec args, CekCtx*, auto op) -> Value
      {
      auto lc = [](char32_t c) -> char32_t { return c < 128 ? (char32_t)std::tolower((int)c) : c; };
      for (size_t i = 0; i + 1 < args.size(); ++i)
         if (!op(lc(as_char(args[i])), lc(as_char(args[i+1])))) return make_bool(false);
      return make_bool(true);
      };
   def(env, "char-ci=?",  [char_ci_cmp](ArgVec a, CekCtx* c){ return char_ci_cmp(a, c, [](char32_t x, char32_t y){ return x == y; }); }, 2, -1);
   def(env, "char-ci<?",  [char_ci_cmp](ArgVec a, CekCtx* c){ return char_ci_cmp(a, c, [](char32_t x, char32_t y){ return x < y; }); }, 2, -1);
   def(env, "char-ci<=?", [char_ci_cmp](ArgVec a, CekCtx* c){ return char_ci_cmp(a, c, [](char32_t x, char32_t y){ return x <= y; }); }, 2, -1);
   def(env, "char-ci>?",  [char_ci_cmp](ArgVec a, CekCtx* c){ return char_ci_cmp(a, c, [](char32_t x, char32_t y){ return x > y; }); }, 2, -1);
   def(env, "char-ci>=?", [char_ci_cmp](ArgVec a, CekCtx* c){ return char_ci_cmp(a, c, [](char32_t x, char32_t y){ return x >= y; }); }, 2, -1);
   }

// ── Vectors ───────────────────────────────────────────────────────────────────
static void register_vectors(Environment* env)
   {
   def(env, "make-vector", [](ArgVec args, CekCtx*) -> Value
      {
      int64_t k = as_fixnum(args[0]);
      if (k < 0) throw SchemeError("make-vector: negative length");
      Value fill = (args.size() >= 2) ? args[1] : make_bool(false);
      return make_vector(gc_alloc_vector((size_t)k, fill));
      }, 1, 2);

   def(env, "vector", [](ArgVec args, CekCtx*) -> Value
      {
      SchemeVector* v = gc_alloc_vector(0);
      for (auto& a : args) v->elements.push_back(a);
      return make_vector(v);
      }, 0, -1);

   def(env, "vector-length", [](ArgVec args, CekCtx*)
      { return make_fixnum((int64_t)as_vector(args[0])->elements.size()); }, 1, 1);

   def(env, "vector-ref", [](ArgVec args, CekCtx*) -> Value
      {
      auto* v = as_vector(args[0]); int64_t k = as_fixnum(args[1]);
      if (k < 0 || (size_t)k >= v->elements.size()) throw SchemeError("vector-ref: index out of range");
      return v->elements[(size_t)k];
      }, 2, 2);

   def(env, "vector-set!", [](ArgVec args, CekCtx*) -> Value
      {
      auto* v = as_vector(args[0]); int64_t k = as_fixnum(args[1]);
      if (k < 0 || (size_t)k >= v->elements.size()) throw SchemeError("vector-set!: index out of range");
      v->elements[(size_t)k] = args[2];
      return make_unspecified();
      }, 3, 3);

   def(env, "vector-fill!", [](ArgVec args, CekCtx*) -> Value
      {
      auto* v = as_vector(args[0]); Value fill = args[1];
      size_t start = (args.size() >= 3) ? (size_t)as_fixnum(args[2]) : 0;
      size_t end   = (args.size() >= 4) ? (size_t)as_fixnum(args[3]) : v->elements.size();
      for (size_t i = start; i < end; ++i) v->elements[i] = fill;
      return make_unspecified();
      }, 2, 4);

   def(env, "vector->list", [](ArgVec args, CekCtx*) -> Value
      {
      auto* v = as_vector(args[0]);
      size_t start = (args.size() >= 2) ? (size_t)as_fixnum(args[1]) : 0;
      size_t end   = (args.size() >= 3) ? (size_t)as_fixnum(args[2]) : v->elements.size();
      Value r = make_nil();
      for (int i = (int)end-1; i >= (int)start; --i) { auto* c = gc_alloc_cons(); c->car = v->elements[i]; c->cdr = r; r = make_cons(c); }
      return r;
      }, 1, 3);

   def(env, "list->vector", [](ArgVec args, CekCtx*) -> Value
      {
      SchemeVector* v = gc_alloc_vector(0);
      Value lst = args[0];
      while (is_cons(lst)) { v->elements.push_back(car(lst)); lst = cdr(lst); }
      return make_vector(v);
      }, 1, 1);

   def(env, "vector-copy", [](ArgVec args, CekCtx*) -> Value
      {
      auto* v = as_vector(args[0]);
      size_t start = (args.size() >= 2) ? (size_t)as_fixnum(args[1]) : 0;
      size_t end   = (args.size() >= 3) ? (size_t)as_fixnum(args[2]) : v->elements.size();
      SchemeVector* nv = gc_alloc_vector(0);
      for (size_t i = start; i < end; ++i) nv->elements.push_back(v->elements[i]);
      return make_vector(nv);
      }, 1, 3);

   def(env, "vector-copy!", [](ArgVec args, CekCtx*) -> Value
      {
      auto* to = as_vector(args[0]); int64_t at = as_fixnum(args[1]);
      auto* from = as_vector(args[2]);
      size_t start = (args.size() >= 4) ? (size_t)as_fixnum(args[3]) : 0;
      size_t end   = (args.size() >= 5) ? (size_t)as_fixnum(args[4]) : from->elements.size();
      for (size_t i = start; i < end; ++i) to->elements[(size_t)at++] = from->elements[i];
      return make_unspecified();
      }, 3, 5);

   def(env, "vector-append", [](ArgVec args, CekCtx*) -> Value
      {
      SchemeVector* v = gc_alloc_vector(0);
      for (auto& a : args) { auto* src = as_vector(a); for (auto& e : src->elements) v->elements.push_back(e); }
      return make_vector(v);
      }, 0, -1);

   def(env, "vector-map", [](ArgVec args, CekCtx* ctx) -> Value
      {
      Value proc = args[0];
      std::vector<SchemeVector*> vecs;
      for (size_t i = 1; i < args.size(); ++i) vecs.push_back(as_vector(args[i]));
      size_t n = vecs.empty() ? 0 : vecs[0]->elements.size();
      SchemeVector* r = gc_alloc_vector(0);
      for (size_t i = 0; i < n; ++i)
         {
         std::vector<Value> row;
         for (auto* v : vecs) row.push_back(v->elements[i]);
         r->elements.push_back(apply_scheme_proc(proc, row, *ctx));
         }
      return make_vector(r);
      }, 2, -1);

   def(env, "vector-for-each", [](ArgVec args, CekCtx* ctx) -> Value
      {
      Value proc = args[0];
      std::vector<SchemeVector*> vecs;
      for (size_t i = 1; i < args.size(); ++i) vecs.push_back(as_vector(args[i]));
      size_t n = vecs.empty() ? 0 : vecs[0]->elements.size();
      for (size_t i = 0; i < n; ++i)
         {
         std::vector<Value> row;
         for (auto* v : vecs) row.push_back(v->elements[i]);
         apply_scheme_proc(proc, row, *ctx);
         }
      return make_unspecified();
      }, 2, -1);

   def(env, "string->vector", [](ArgVec args, CekCtx*) -> Value
      {
      const std::string& s = as_string(args[0])->data;
      size_t n = utf8_codepoint_count(s);
      size_t start = (args.size() >= 2) ? (size_t)as_fixnum(args[1]) : 0;
      size_t end   = (args.size() >= 3) ? (size_t)as_fixnum(args[2]) : n;
      SchemeVector* v = gc_alloc_vector(0);
      size_t pos = 0;
      for (size_t i = 0; i < n; ++i)
         {
         char32_t cp = utf8_decode_one(s, pos);
         if (i >= start && i < end) v->elements.push_back(make_char(cp));
         }
      return make_vector(v);
      }, 1, 3);

   def(env, "vector->string", [](ArgVec args, CekCtx*) -> Value
      {
      auto* v = as_vector(args[0]);
      size_t start = (args.size() >= 2) ? (size_t)as_fixnum(args[1]) : 0;
      size_t end   = (args.size() >= 3) ? (size_t)as_fixnum(args[2]) : v->elements.size();
      std::string s;
      for (size_t i = start; i < end; ++i) s += codepoint_to_utf8(as_char(v->elements[i]));
      return make_string(gc_alloc_string(s));
      }, 1, 3);
   }

// ── Bytevectors ───────────────────────────────────────────────────────────────
static void register_bytevectors(Environment* env)
   {
   def(env, "make-bytevector", [](ArgVec args, CekCtx*) -> Value
      {
      int64_t k = as_fixnum(args[0]); if (k < 0) throw SchemeError("make-bytevector: negative length");
      uint8_t fill = (args.size() >= 2) ? (uint8_t)as_fixnum(args[1]) : 0;
      return make_bytevector(gc_alloc_bytevector((size_t)k, fill));
      }, 1, 2);

   def(env, "bytevector", [](ArgVec args, CekCtx*) -> Value
      {
      SchemeBytevector* bv = gc_alloc_bytevector(0);
      for (auto& a : args) bv->data.push_back((uint8_t)as_fixnum(a));
      return make_bytevector(bv);
      }, 0, -1);

   def(env, "bytevector-length", [](ArgVec args, CekCtx*)
      { return make_fixnum((int64_t)as_bytevector(args[0])->data.size()); }, 1, 1);

   def(env, "bytevector-u8-ref", [](ArgVec args, CekCtx*) -> Value
      {
      auto* bv = as_bytevector(args[0]); int64_t k = as_fixnum(args[1]);
      if (k < 0 || (size_t)k >= bv->data.size()) throw SchemeError("bytevector-u8-ref: index out of range");
      return make_fixnum((int64_t)bv->data[(size_t)k]);
      }, 2, 2);

   def(env, "bytevector-u8-set!", [](ArgVec args, CekCtx*) -> Value
      {
      auto* bv = as_bytevector(args[0]); int64_t k = as_fixnum(args[1]);
      if (k < 0 || (size_t)k >= bv->data.size()) throw SchemeError("bytevector-u8-set!: index out of range");
      bv->data[(size_t)k] = (uint8_t)as_fixnum(args[2]);
      return make_unspecified();
      }, 3, 3);

   def(env, "bytevector-copy", [](ArgVec args, CekCtx*) -> Value
      {
      auto* bv = as_bytevector(args[0]);
      size_t start = (args.size() >= 2) ? (size_t)as_fixnum(args[1]) : 0;
      size_t end   = (args.size() >= 3) ? (size_t)as_fixnum(args[2]) : bv->data.size();
      SchemeBytevector* nb = gc_alloc_bytevector(0);
      nb->data.assign(bv->data.begin() + start, bv->data.begin() + end);
      return make_bytevector(nb);
      }, 1, 3);

   def(env, "bytevector-copy!", [](ArgVec args, CekCtx*) -> Value
      {
      auto* to = as_bytevector(args[0]); int64_t at = as_fixnum(args[1]);
      auto* from = as_bytevector(args[2]);
      size_t start = (args.size() >= 4) ? (size_t)as_fixnum(args[3]) : 0;
      size_t end   = (args.size() >= 5) ? (size_t)as_fixnum(args[4]) : from->data.size();
      for (size_t i = start; i < end; ++i) to->data[(size_t)at++] = from->data[i];
      return make_unspecified();
      }, 3, 5);

   def(env, "bytevector-append", [](ArgVec args, CekCtx*) -> Value
      {
      SchemeBytevector* nb = gc_alloc_bytevector(0);
      for (auto& a : args) { auto* bv = as_bytevector(a); nb->data.insert(nb->data.end(), bv->data.begin(), bv->data.end()); }
      return make_bytevector(nb);
      }, 0, -1);

   def(env, "utf8->string", [](ArgVec args, CekCtx*) -> Value
      {
      auto* bv = as_bytevector(args[0]);
      size_t start = (args.size() >= 2) ? (size_t)as_fixnum(args[1]) : 0;
      size_t end   = (args.size() >= 3) ? (size_t)as_fixnum(args[2]) : bv->data.size();
      std::string s(bv->data.begin() + start, bv->data.begin() + end);
      return make_string(gc_alloc_string(s));
      }, 1, 3);

   def(env, "string->utf8", [](ArgVec args, CekCtx*) -> Value
      {
      const std::string& s = as_string(args[0])->data;
      size_t n = utf8_codepoint_count(s);
      size_t start = (args.size() >= 2) ? (size_t)as_fixnum(args[1]) : 0;
      size_t end   = (args.size() >= 3) ? (size_t)as_fixnum(args[2]) : n;
      std::string sub; size_t pos = 0;
      for (size_t i = 0; i < n; ++i)
         {
         size_t s0 = pos; char32_t cp = utf8_decode_one(s, pos);
         if (i >= start && i < end) sub += s.substr(s0, pos - s0);
         (void)cp;
         }
      SchemeBytevector* bv = gc_alloc_bytevector(0);
      bv->data.assign(sub.begin(), sub.end());
      return make_bytevector(bv);
      }, 1, 3);

   def(env, "bytevector-ieee-double-native-ref", [](ArgVec args, CekCtx*) -> Value
      {
      auto* bv = as_bytevector(args[0]); size_t k = (size_t)as_fixnum(args[1]);
      if (k + 8 > bv->data.size()) throw SchemeError("bytevector-ieee-double-native-ref: out of range");
      double d; std::memcpy(&d, bv->data.data() + k, 8);
      return make_flonum(d);
      }, 2, 2);

   def(env, "bytevector-ieee-double-native-set!", [](ArgVec args, CekCtx*) -> Value
      {
      auto* bv = as_bytevector(args[0]); size_t k = (size_t)as_fixnum(args[1]); double d = as_flonum(args[2]);
      if (k + 8 > bv->data.size()) throw SchemeError("bytevector-ieee-double-native-set!: out of range");
      std::memcpy(bv->data.data() + k, &d, 8);
      return make_unspecified();
      }, 3, 3);
   }

// ── Ports ─────────────────────────────────────────────────────────────────────
static void register_ports(Environment* env)
   {
   // ── Current port parameters ───────────────────────────────────────────────
   // These are SchemeParameter objects; parameterize rebinds them.
   def(env, "current-input-port", [](ArgVec, CekCtx*) -> Value
      { return g_current_input_port; }, 0, 0);
   def(env, "current-output-port", [](ArgVec, CekCtx*) -> Value
      { return g_current_output_port; }, 0, 0);
   def(env, "current-error-port", [](ArgVec, CekCtx*) -> Value
      { return g_current_error_port; }, 0, 0);

   // ── Open ports ────────────────────────────────────────────────────────────
   def(env, "open-input-file", [](ArgVec args, CekCtx*) -> Value
      {
      const std::string& path = as_string(args[0])->data;
      FILE* fp = fopen(path.c_str(), "rb");
      if (!fp) raise_file_error("open-input-file: cannot open: " + path);
      SchemePort* p = gc_alloc_port();
      p->is_input = true; p->is_output = false; p->is_text = true; p->is_open = true;
      p->kind = SchemePort::Kind::FileIn; p->fp = fp; p->owns_fp = true; p->name = path;
      return make_port(p);
      }, 1, 1);

   def(env, "open-output-file", [](ArgVec args, CekCtx*) -> Value
      {
      const std::string& path = as_string(args[0])->data;
      FILE* fp = fopen(path.c_str(), "wb");
      if (!fp) raise_file_error("open-output-file: cannot open: " + path);
      SchemePort* p = gc_alloc_port();
      p->is_input = false; p->is_output = true; p->is_text = true; p->is_open = true;
      p->kind = SchemePort::Kind::FileOut; p->fp = fp; p->owns_fp = true; p->name = path;
      return make_port(p);
      }, 1, 1);

   def(env, "open-input-string", [](ArgVec args, CekCtx*) -> Value
      {
      SchemePort* p = gc_alloc_port();
      p->is_input = true; p->is_output = false; p->is_text = true; p->is_open = true;
      p->kind = SchemePort::Kind::StrIn;
      p->str_buf = as_string(args[0])->data; p->str_pos = 0; p->name = "<string-port>";
      return make_port(p);
      }, 1, 1);

   def(env, "open-output-string", [](ArgVec, CekCtx*) -> Value
      {
      SchemePort* p = gc_alloc_port();
      p->is_input = false; p->is_output = true; p->is_text = true; p->is_open = true;
      p->kind = SchemePort::Kind::StrOut; p->name = "<string-output-port>";
      return make_port(p);
      }, 0, 0);

   def(env, "get-output-string", [](ArgVec args, CekCtx*) -> Value
      {
      SchemePort* p = as_port(args[0]);
      if (p->kind != SchemePort::Kind::StrOut) throw SchemeError("get-output-string: not a string output port");
      return make_string(gc_alloc_string(p->out_buf));
      }, 1, 1);

   def(env, "open-input-bytevector", [](ArgVec args, CekCtx*) -> Value
      {
      SchemePort* p = gc_alloc_port();
      p->is_input = true; p->is_output = false; p->is_text = false; p->is_open = true;
      p->kind = SchemePort::Kind::BvIn;
      p->bv_buf = as_bytevector(args[0])->data; p->bv_pos = 0; p->name = "<bytevector-port>";
      return make_port(p);
      }, 1, 1);

   def(env, "open-output-bytevector", [](ArgVec, CekCtx*) -> Value
      {
      SchemePort* p = gc_alloc_port();
      p->is_input = false; p->is_output = true; p->is_text = false; p->is_open = true;
      p->kind = SchemePort::Kind::BvOut; p->name = "<bytevector-output-port>";
      return make_port(p);
      }, 0, 0);

   def(env, "get-output-bytevector", [](ArgVec args, CekCtx*) -> Value
      {
      SchemePort* p = as_port(args[0]);
      if (p->kind != SchemePort::Kind::BvOut) throw SchemeError("get-output-bytevector: not a bytevector output port");
      SchemeBytevector* bv = gc_alloc_bytevector(0);
      bv->data = p->bv_buf;
      return make_bytevector(bv);
      }, 1, 1);

   // ── Close ports ───────────────────────────────────────────────────────────
   def(env, "close-port", [](ArgVec args, CekCtx*) -> Value
      { as_port(args[0])->is_open = false; return make_unspecified(); }, 1, 1);
   def(env, "close-input-port",  [](ArgVec args, CekCtx*) -> Value
      { as_port(args[0])->is_open = false; return make_unspecified(); }, 1, 1);
   def(env, "close-output-port", [](ArgVec args, CekCtx*) -> Value
      { as_port(args[0])->is_open = false; return make_unspecified(); }, 1, 1);

   // ── Read operations ───────────────────────────────────────────────────────
   def(env, "read-char", [](ArgVec args, CekCtx* ctx) -> Value
      {
      SchemePort* p;
      if (args.empty())
         {
         p = current_input_port_raw();
         if (!p) p = make_stdio_port(true, stdin, "<stdin>");
         }
      else p = check_open_input_port(args[0], "read-char");
      int32_t ch = port_read_char(p);
      if (ch < 0) return make_eof();
      return make_char((char32_t)ch);
      }, 0, 1);

   def(env, "peek-char", [](ArgVec args, CekCtx* ctx) -> Value
      {
      SchemePort* p;
      if (args.empty()) { p = current_input_port_raw(); if (!p) p = make_stdio_port(true, stdin, "<stdin>"); }
      else p = check_open_input_port(args[0], "peek-char");
      int32_t ch = port_peek_char(p);
      if (ch < 0) return make_eof();
      return make_char((char32_t)ch);
      }, 0, 1);

   def(env, "char-ready?", [](ArgVec args, CekCtx*) -> Value
      {
      SchemePort* p;
      if (args.empty()) { p = current_input_port_raw(); if (!p) return make_bool(false); }
      else { if (!is_port(args[0])) return make_bool(false); p = as_port(args[0]); }
      if (!p->is_open) return make_bool(false);
      // Always say ready for string ports and file ports; may block for stdio
      return make_bool(p->kind == SchemePort::Kind::StrIn || p->kind == SchemePort::Kind::FileIn);
      }, 0, 1);

   def(env, "read-byte", [](ArgVec args, CekCtx*) -> Value
      {
      SchemePort* p;
      if (args.empty()) { p = current_input_port_raw(); if (!p) p = make_stdio_port(true, stdin, "<stdin>"); }
      else p = check_open_input_port(args[0], "read-byte");
      int b = port_read_byte(p);
      if (b < 0) return make_eof();
      return make_fixnum((int64_t)b);
      }, 0, 1);

   def(env, "peek-byte", [](ArgVec args, CekCtx*) -> Value
      {
      SchemePort* p;
      if (args.empty()) { p = current_input_port_raw(); if (!p) p = make_stdio_port(true, stdin, "<stdin>"); }
      else p = check_open_input_port(args[0], "peek-byte");
      int b = port_peek_byte(p);
      if (b < 0) return make_eof();
      return make_fixnum((int64_t)b);
      }, 0, 1);

   def(env, "byte-ready?", [](ArgVec, CekCtx*) -> Value { return make_bool(true); }, 0, 1);

   def(env, "read-line", [](ArgVec args, CekCtx*) -> Value
      {
      SchemePort* p;
      if (args.empty()) { p = current_input_port_raw(); if (!p) p = make_stdio_port(true, stdin, "<stdin>"); }
      else p = check_open_input_port(args[0], "read-line");
      std::string line;
      while (true)
         {
         int32_t ch = port_read_char(p);
         if (ch < 0) { if (line.empty()) return make_eof(); break; }
         if (ch == '\n') break;
         line += codepoint_to_utf8((char32_t)ch);
         }
      return make_string(gc_alloc_string(line));
      }, 0, 1);

   def(env, "read-string", [](ArgVec args, CekCtx*) -> Value
      {
      int64_t k = as_fixnum(args[0]);
      SchemePort* p;
      if (args.size() >= 2) p = check_open_input_port(args[1], "read-string");
      else { p = current_input_port_raw(); if (!p) p = make_stdio_port(true, stdin, "<stdin>"); }
      std::string s;
      for (int64_t i = 0; i < k; ++i)
         {
         int32_t ch = port_read_char(p);
         if (ch < 0) { if (s.empty()) return make_eof(); break; }
         s += codepoint_to_utf8((char32_t)ch);
         }
      return make_string(gc_alloc_string(s));
      }, 1, 2);

   def(env, "read-u8-list", [](ArgVec args, CekCtx*) -> Value
      {
      // Read k bytes from binary port
      int64_t k = as_fixnum(args[0]);
      SchemePort* p = check_open_input_port(args[1], "read-u8-list");
      SchemeBytevector* bv = gc_alloc_bytevector(0);
      for (int64_t i = 0; i < k; ++i) { int b = port_read_byte(p); if (b < 0) break; bv->data.push_back((uint8_t)b); }
      return make_bytevector(bv);
      }, 2, 2);

   def(env, "read-bytevector", [](ArgVec args, CekCtx*) -> Value
      {
      int64_t k = as_fixnum(args[0]);
      SchemePort* p;
      if (args.size() >= 2) p = check_open_input_port(args[1], "read-bytevector");
      else { p = current_input_port_raw(); if (!p) p = make_stdio_port(false, stdin, "<stdin>"); }
      SchemeBytevector* bv = gc_alloc_bytevector(0);
      for (int64_t i = 0; i < k; ++i) { int b = port_read_byte(p); if (b < 0) { if (bv->data.empty()) return make_eof(); break; } bv->data.push_back((uint8_t)b); }
      return make_bytevector(bv);
      }, 1, 2);

   def(env, "read-bytevector!", [](ArgVec args, CekCtx*) -> Value
      {
      auto* bv = as_bytevector(args[0]);
      SchemePort* p;
      if (args.size() >= 2) p = check_open_input_port(args[1], "read-bytevector!"); else { p = current_input_port_raw(); if (!p) p = make_stdio_port(false, stdin, "<stdin>"); }
      size_t start = (args.size() >= 3) ? (size_t)as_fixnum(args[2]) : 0;
      size_t end   = (args.size() >= 4) ? (size_t)as_fixnum(args[3]) : bv->data.size();
      int64_t count = 0;
      for (size_t i = start; i < end; ++i) { int b = port_read_byte(p); if (b < 0) break; bv->data[i] = (uint8_t)b; ++count; }
      if (count == 0) return make_eof();
      return make_fixnum(count);
      }, 1, 4);

   def(env, "read", [](ArgVec args, CekCtx*) -> Value
      {
      SchemePort* p;
      if (args.empty()) { p = current_input_port_raw(); if (!p) p = make_stdio_port(true, stdin, "<stdin>"); }
      else p = check_open_input_port(args[0], "read");
      // For string ports: parse from remaining buffer
      std::string src;
      if (p->kind == SchemePort::Kind::StrIn)
         src = p->str_buf.substr(p->str_pos);
      else if (p->kind == SchemePort::Kind::FileIn || p->kind == SchemePort::Kind::Stdio)
         {
         // Read lines until we get a parseable expression or EOF
         // Simple approach: read all remaining content
         if (p->fp)
            {
            char buf[4096]; size_t n;
            while ((n = fread(buf, 1, sizeof(buf), p->fp)) > 0) src += std::string(buf, n);
            }
         }
      else return make_eof();
      if (src.empty()) return make_eof();
      Parser parser(src);
      auto result = parser.next();
      if (!result) return make_eof();
      if (p->kind == SchemePort::Kind::StrIn)
         p->str_pos = p->str_buf.size() - (src.size() - parser.source_offset());
      return *result;
      }, 0, 1);

   // ── Write operations ──────────────────────────────────────────────────────
   def(env, "write-char", [](ArgVec args, CekCtx* ctx) -> Value
      {
      char32_t cp = as_char(args[0]);
      SchemePort* p = nullptr;
      if (args.size() >= 2) p = check_open_output_port(args[1], "write-char");
      if (p) port_write_char(p, cp);
      else
         {
         auto* op = current_output_port_raw(ctx);
         if (op) port_write_char(op, cp);
         else prim_out() << codepoint_to_utf8(cp) << std::flush;
         }
      return make_unspecified();
      }, 1, 2);

   def(env, "write-string", [](ArgVec args, CekCtx* ctx) -> Value
      {
      const std::string& s = as_string(args[0])->data;
      SchemePort* p = nullptr;
      if (args.size() >= 2) p = check_open_output_port(args[1], "write-string");
      if (p) port_write_string(p, s);
      else
         {
         auto* op = current_output_port_raw(ctx);
         if (op) port_write_string(op, s);
         else prim_out() << s << std::flush;
         }
      return make_unspecified();
      }, 1, 4);

   def(env, "write-byte", [](ArgVec args, CekCtx*) -> Value
      {
      uint8_t byte = (uint8_t)as_fixnum(args[0]);
      if (args.size() >= 2)
         {
         SchemePort* p = check_open_output_port(args[1], "write-byte");
         if (p->kind == SchemePort::Kind::BvOut) p->bv_buf.push_back(byte);
         else if (p->fp) fputc(byte, p->fp);
         }
      else prim_out().put((char)byte);
      return make_unspecified();
      }, 1, 2);

   def(env, "write-u8", [](ArgVec args, CekCtx*) -> Value
      {
      uint8_t byte = (uint8_t)as_fixnum(args[0]);
      if (args.size() >= 2)
         {
         SchemePort* p = check_open_output_port(args[1], "write-u8");
         if (p->kind == SchemePort::Kind::BvOut) p->bv_buf.push_back(byte);
         else if (p->fp) fputc(byte, p->fp);
         }
      else prim_out().put((char)byte);
      return make_unspecified();
      }, 1, 2);

   def(env, "write-bytevector", [](ArgVec args, CekCtx*) -> Value
      {
      auto* bv = as_bytevector(args[0]);
      size_t start = (args.size() >= 3) ? (size_t)as_fixnum(args[2]) : 0;
      size_t end   = (args.size() >= 4) ? (size_t)as_fixnum(args[3]) : bv->data.size();
      SchemePort* p = (args.size() >= 2) ? check_open_output_port(args[1], "write-bytevector") : nullptr;
      for (size_t i = start; i < end; ++i)
         {
         uint8_t b = bv->data[i];
         if (p) { if (p->kind == SchemePort::Kind::BvOut) p->bv_buf.push_back(b); else if (p->fp) fputc(b, p->fp); }
         else prim_out().put((char)b);
         }
      return make_unspecified();
      }, 1, 4);

   def(env, "newline", [](ArgVec args, CekCtx* ctx) -> Value
      {
      SchemePort* p = nullptr;
      if (!args.empty()) p = check_open_output_port(args[0], "newline");
      if (p) port_write_char(p, U'\n');
      else
         {
         auto* op = current_output_port_raw(ctx);
         if (op) port_write_char(op, U'\n');
         else prim_out() << '\n' << std::flush;
         }
      return make_unspecified();
      }, 0, 1);

   def(env, "display", [](ArgVec args, CekCtx* ctx) -> Value
      {
      std::string s = value_to_string(args[0], true);
      SchemePort* p = (args.size() >= 2) ? check_open_output_port(args[1], "display") : nullptr;
      if (p) port_write_string(p, s);
      else
         {
         auto* op = current_output_port_raw(ctx);
         if (op) port_write_string(op, s);
         else prim_out() << s << std::flush;
         }
      return make_unspecified();
      }, 1, 2);

   def(env, "write", [](ArgVec args, CekCtx* ctx) -> Value
      {
      std::string s = value_to_string(args[0], false);
      SchemePort* p = (args.size() >= 2) ? check_open_output_port(args[1], "write") : nullptr;
      if (p) port_write_string(p, s);
      else
         {
         auto* op = current_output_port_raw(ctx);
         if (op) port_write_string(op, s);
         else prim_out() << s << std::flush;
         }
      return make_unspecified();
      }, 1, 2);

   def(env, "write-shared", [](ArgVec args, CekCtx* ctx) -> Value
      {
      std::string s = write_shared_to_string(args[0]);
      SchemePort* p = (args.size() >= 2) ? check_open_output_port(args[1], "write-shared") : nullptr;
      if (p) port_write_string(p, s);
      else
         {
         auto* op = current_output_port_raw(ctx);
         if (op) port_write_string(op, s);
         else prim_out() << s << std::flush;
         }
      return make_unspecified();
      }, 1, 2);

   def(env, "flush-output-port", [](ArgVec args, CekCtx* ctx) -> Value
      {
      SchemePort* p = (args.empty()) ? current_output_port_raw(ctx) : (is_port(args[0]) ? as_port(args[0]) : nullptr);
      if (p && p->fp) fflush(p->fp);
      else prim_out().flush();
      return make_unspecified();
      }, 0, 1);

   def(env, "with-port", [](ArgVec args, CekCtx* ctx) -> Value
      {
      check_open_input_port(args[0], "with-port");
      return apply_scheme_proc(args[1], {}, *ctx);
      }, 2, 2);

   def(env, "call-with-port", [](ArgVec args, CekCtx* ctx) -> Value
      {
      Value port = args[0]; Value proc = args[1];
      Value result = apply_scheme_proc(proc, {port}, *ctx);
      if (is_port(port)) as_port(port)->is_open = false;
      return result;
      }, 2, 2);

   def(env, "with-input-from-file", [](ArgVec args, CekCtx* ctx) -> Value
      {
      const std::string& path = as_string(args[0])->data;
      FILE* fp = fopen(path.c_str(), "rb");
      if (!fp) raise_file_error("with-input-from-file: cannot open: " + path);
      SchemePort* p = gc_alloc_port();
      p->is_input = true; p->is_text = true; p->is_open = true;
      p->kind = SchemePort::Kind::FileIn; p->fp = fp; p->owns_fp = true; p->name = path;
      Value port_val = make_port(p);
      // Temporarily set current-input-port
      Value old = g_current_input_port;
      if (is_parameter(g_current_input_port)) as_parameter(g_current_input_port)->current = port_val;
      Value result;
      try { result = apply_scheme_proc(args[1], {}, *ctx); } catch (...) { as_port(port_val)->is_open = false; if (is_parameter(g_current_input_port)) as_parameter(g_current_input_port)->current = is_parameter(old) ? as_parameter(old)->current : old; throw; }
      as_port(port_val)->is_open = false;
      if (is_parameter(g_current_input_port)) as_parameter(g_current_input_port)->current = is_parameter(old) ? as_parameter(old)->current : old;
      return result;
      }, 2, 2);

   def(env, "with-output-to-file", [](ArgVec args, CekCtx* ctx) -> Value
      {
      const std::string& path = as_string(args[0])->data;
      FILE* fp = fopen(path.c_str(), "wb");
      if (!fp) raise_file_error("with-output-to-file: cannot open: " + path);
      SchemePort* p = gc_alloc_port();
      p->is_output = true; p->is_text = true; p->is_open = true;
      p->kind = SchemePort::Kind::FileOut; p->fp = fp; p->owns_fp = true; p->name = path;
      Value port_val = make_port(p);
      Value old = g_current_output_port;
      if (is_parameter(g_current_output_port)) as_parameter(g_current_output_port)->current = port_val;
      Value result;
      try { result = apply_scheme_proc(args[1], {}, *ctx); } catch (...) { as_port(port_val)->is_open = false; if (is_parameter(g_current_output_port)) as_parameter(g_current_output_port)->current = is_parameter(old) ? as_parameter(old)->current : old; throw; }
      as_port(port_val)->is_open = false;
      if (is_parameter(g_current_output_port)) as_parameter(g_current_output_port)->current = is_parameter(old) ? as_parameter(old)->current : old;
      return result;
      }, 2, 2);

   def(env, "eof-object",  [](ArgVec, CekCtx*) -> Value { return make_eof(); }, 0, 0);

   def(env, "file-exists?", [](ArgVec args, CekCtx*) -> Value
      {
      const std::string& path = as_string(args[0])->data;
      FILE* fp = fopen(path.c_str(), "rb");
      if (fp) { fclose(fp); return make_bool(true); }
      return make_bool(false);
      }, 1, 1);

   def(env, "delete-file", [](ArgVec args, CekCtx*) -> Value
      {
      const std::string& path = as_string(args[0])->data;
      if (remove(path.c_str()) != 0) raise_file_error("delete-file: " + path);
      return make_unspecified();
      }, 1, 1);
   }

// ── Lazy ─────────────────────────────────────────────────────────────────────
static void register_lazy(Environment* env)
   {
   // force is intercepted by the evaluator; stub just in case
   auto stub = [](const char* name) -> BuiltinFn
      {
      return [name](ArgVec, CekCtx*) -> Value
         { throw SchemeError(std::string(name) + ": cannot be called through a re-entering path"); };
      };

   def(env, "force",       stub("force"), 1, 1);
   def(env, "make-promise",[](ArgVec args, CekCtx*) -> Value
      {
      SchemePromise* p = gc_alloc_promise(args[0]);
      p->forced = true;
      p->val    = args[0];
      return make_promise_val(p);
      }, 1, 1);
   def(env, "promise?",    [](ArgVec args, CekCtx*) -> Value { return make_bool(is_promise(args[0])); }, 1, 1);
   def(env, "make-parameter", stub("make-parameter"), 1, 2);
   }

// ── Control stubs (intercepted by evaluator) ──────────────────────────────────
static void register_control_stubs(Environment* env)
   {
   auto stub = [](const char* name) -> BuiltinFn
      {
      return [name](ArgVec, CekCtx*) -> Value
         { throw SchemeError(std::string(name) + ": cannot be called through a re-entering path"); };
      };
   def(env, "error",                         [](ArgVec args, CekCtx*) -> Value
      {
      std::string msg;
      if (!args.empty() && is_string(args[0])) msg = as_string(args[0])->data;
      else if (!args.empty()) msg = value_to_string(args[0]);
      std::vector<Value> irritants(args.begin() + (args.empty() ? 0 : 1), args.end());
      SchemeErrorObject* obj = gc_alloc_error_object();
      obj->message = msg; obj->kind = 0;
      Value tail = make_nil();
      for (int i = (int)irritants.size()-1; i >= 0; --i) { auto* c = gc_alloc_cons(); c->car = irritants[i]; c->cdr = tail; tail = make_cons(c); }
      obj->irritants = tail;
      throw SchemeRaisedException(make_error_object_val(obj), false);
      }, 1, -1);
   def(env, "raise",                         stub("raise"), 1, 1);
   def(env, "raise-continuable",             stub("raise-continuable"), 1, 1);
   def(env, "with-exception-handler",        stub("with-exception-handler"), 2, 2);
   def(env, "call-with-values",              stub("call-with-values"), 2, 2);
   def(env, "call-with-current-continuation",stub("call-with-current-continuation"), 1, 1);
   def(env, "call/cc",                       stub("call/cc"), 1, 1);
   def(env, "dynamic-wind",                  stub("dynamic-wind"), 3, 3);
   def(env, "apply",                         stub("apply"), 2, -1);
   def(env, "eval",                          stub("eval"), 1, 2);
   def(env, "%with-parameters",              stub("%with-parameters"), 3, 3);

   def(env, "values", [](ArgVec args, CekCtx*) -> Value
      {
      if (args.size() == 1) return args[0];
      if (args.empty()) return make_values_val({});
      return make_values_val(std::vector<Value>(args.begin(), args.end()));
      }, 0, -1);
   }

// ── Meta / environment / records ─────────────────────────────────────────────
static void register_meta(Environment* env)
   {
   def(env, "interaction-environment", [](ArgVec, CekCtx*) -> Value
      {
      throw SchemeError("interaction-environment: not available in primitives context");
      }, 0, 0);

   // Error object accessors
   def(env, "error-object-message", [](ArgVec args, CekCtx*) -> Value
      {
      if (!is_error_object(args[0])) throw SchemeTypeError("error-object-message: not an error object");
      return make_string(gc_alloc_string(as_error_object(args[0])->message));
      }, 1, 1);

   def(env, "error-object-irritants", [](ArgVec args, CekCtx*) -> Value
      {
      if (!is_error_object(args[0])) throw SchemeTypeError("error-object-irritants: not an error object");
      return as_error_object(args[0])->irritants;
      }, 1, 1);

   def(env, "file-error?", [](ArgVec args, CekCtx*) -> Value
      { return make_bool(is_error_object(args[0]) && as_error_object(args[0])->kind == 1); }, 1, 1);

   def(env, "read-error?", [](ArgVec args, CekCtx*) -> Value
      { return make_bool(is_error_object(args[0]) && as_error_object(args[0])->kind == 2); }, 1, 1);

   // Record primitives: emitted by the expander for define-record-type
   def(env, "%make-record-type", [](ArgVec args, CekCtx*) -> Value
      {
      if (!is_symbol(args[0])) throw SchemeTypeError("%make-record-type: first argument must be a symbol");
      std::string name = symbol_name(as_symbol_id(args[0]));
      std::vector<std::string> fields;
      Value cur = args[1];
      while (is_cons(cur)) { if (!is_symbol(car(cur))) throw SchemeTypeError("%make-record-type: field names must be symbols"); fields.push_back(symbol_name(as_symbol_id(car(cur)))); cur = cdr(cur); }
      return alloc_record_type_descriptor(name, fields);
      }, 2, 2);

   def(env, "%make-record", [](ArgVec args, CekCtx*) -> Value
      {
      SchemeRecordType* rt = unwrap_record_type(args[0], "%make-record");
      SchemeRecord* r = gc_alloc_record();
      r->type = rt;
      Value cur = args[1];
      while (is_cons(cur)) { r->fields.push_back(car(cur)); cur = cdr(cur); }
      return make_record_val(r);
      }, 2, 2);

   def(env, "%record-of-type?", [](ArgVec args, CekCtx*) -> Value
      {
      SchemeRecordType* rt = unwrap_record_type(args[1], "%record-of-type?");
      if (!is_record(args[0])) return make_bool(false);
      return make_bool(as_record(args[0])->type == rt);
      }, 2, 2);

   def(env, "%record-ref", [](ArgVec args, CekCtx*) -> Value
      {
      SchemeRecordType* rt = unwrap_record_type(args[1], "%record-ref");
      if (!is_record(args[0]) || as_record(args[0])->type != rt) throw SchemeTypeError(std::string("%record-ref: wrong record type: ") + rt->name);
      size_t idx = (size_t)as_fixnum(args[2]);
      if (idx >= as_record(args[0])->fields.size()) throw SchemeError("%record-ref: index out of range");
      return as_record(args[0])->fields[idx];
      }, 3, 3);

   def(env, "%record-set!", [](ArgVec args, CekCtx*) -> Value
      {
      SchemeRecordType* rt = unwrap_record_type(args[1], "%record-set!");
      if (!is_record(args[0]) || as_record(args[0])->type != rt) throw SchemeTypeError(std::string("%record-set!: wrong record type: ") + rt->name);
      size_t idx = (size_t)as_fixnum(args[2]);
      if (idx >= as_record(args[0])->fields.size()) throw SchemeError("%record-set!: index out of range");
      as_record(args[0])->fields[idx] = args[3];
      return make_unspecified();
      }, 4, 4);

   def(env, "%make-record-accessor", [](ArgVec args, CekCtx*) -> Value
      {
      SchemeRecordType* rt = unwrap_record_type(args[0], "%make-record-accessor");
      int64_t idx = as_fixnum(args[1]);
      std::string name = (is_symbol(args[2])) ? symbol_name(as_symbol_id(args[2])) : "accessor";
      auto b = std::make_unique<Builtin>(name, [rt, idx, name](ArgVec a, CekCtx*) -> Value
         {
         if (!is_record(a[0]) || as_record(a[0])->type != rt) throw SchemeTypeError(name + ": wrong type");
         return as_record(a[0])->fields[(size_t)idx];
         });
      b->min_args = 1; b->max_args = 1;
      g_dynamic_builtins.push_back(std::move(b));
      return make_builtin(g_dynamic_builtins.back().get());
      }, 3, 3);

   def(env, "%make-record-mutator", [](ArgVec args, CekCtx*) -> Value
      {
      SchemeRecordType* rt = unwrap_record_type(args[0], "%make-record-mutator");
      int64_t idx = as_fixnum(args[1]);
      std::string name = (is_symbol(args[2])) ? symbol_name(as_symbol_id(args[2])) : "mutator";
      auto b = std::make_unique<Builtin>(name, [rt, idx, name](ArgVec a, CekCtx*) -> Value
         {
         if (!is_record(a[0]) || as_record(a[0])->type != rt) throw SchemeTypeError(name + ": wrong type");
         as_record(a[0])->fields[(size_t)idx] = a[1];
         return make_unspecified();
         });
      b->min_args = 2; b->max_args = 2;
      g_dynamic_builtins.push_back(std::move(b));
      return make_builtin(g_dynamic_builtins.back().get());
      }, 3, 3);

   // Process / time
   def(env, "command-line", [](ArgVec, CekCtx*) -> Value
      {
      Value r = make_nil();
      for (int i = (int)g_command_line_args.size()-1; i >= 0; --i) { auto* c = gc_alloc_cons(); c->car = make_string(gc_alloc_string(g_command_line_args[i])); c->cdr = r; r = make_cons(c); }
      return r;
      }, 0, 0);

   def(env, "exit", [](ArgVec args, CekCtx*) -> Value
      {
      int code = 0;
      if (!args.empty())
         {
         if (is_bool(args[0])) code = as_bool(args[0]) ? 0 : 1;
         else if (is_fixnum(args[0])) code = (int)as_fixnum(args[0]);
         }
      std::exit(code);
      }, 0, 1);

   def(env, "emergency-exit", [](ArgVec args, CekCtx*) -> Value
      {
      int code = 0;
      if (!args.empty())
         {
         if (is_bool(args[0])) code = as_bool(args[0]) ? 0 : 1;
         else if (is_fixnum(args[0])) code = (int)as_fixnum(args[0]);
         }
      _Exit(code);
      }, 0, 1);

   def(env, "get-environment-variable", [](ArgVec args, CekCtx*) -> Value
      {
      const char* val = std::getenv(as_string(args[0])->data.c_str());
      if (!val) return make_bool(false);
      return make_string(gc_alloc_string(val));
      }, 1, 1);

   def(env, "get-environment-variables", [](ArgVec, CekCtx*) -> Value
      {
      Value r = make_nil();
#ifdef _WIN32
      // Windows: iterate GetEnvironmentStrings
      // Simple fallback: return empty list
#else
      extern char** environ;
      if (environ)
         {
         std::vector<std::pair<std::string,std::string>> pairs;
         for (char** e = environ; *e; ++e)
            {
            std::string kv(*e);
            auto eq = kv.find('=');
            if (eq != std::string::npos) pairs.push_back({kv.substr(0, eq), kv.substr(eq+1)});
            }
         for (int i = (int)pairs.size()-1; i >= 0; --i)
            {
            auto* c = gc_alloc_cons();
            auto* inner = gc_alloc_cons();
            inner->car = make_string(gc_alloc_string(pairs[i].first));
            inner->cdr = make_string(gc_alloc_string(pairs[i].second));
            c->car = make_cons(inner); c->cdr = r; r = make_cons(c);
            }
         }
#endif
      return r;
      }, 0, 0);

   def(env, "runtime", [](ArgVec, CekCtx*) -> Value
      { return make_flonum((double)std::clock() / CLOCKS_PER_SEC); }, 0, 0);

   def(env, "current-second", [](ArgVec, CekCtx*) -> Value
      { auto now = std::chrono::system_clock::now(); return make_flonum((double)std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() / 1e6); }, 0, 0);

   def(env, "current-jiffy", [](ArgVec, CekCtx*) -> Value
      { auto now = std::chrono::steady_clock::now(); return make_fixnum((int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()); }, 0, 0);

   def(env, "jiffies-per-second", [](ArgVec, CekCtx*) -> Value { return make_fixnum(1000); }, 0, 0);

   def(env, "load", [](ArgVec args, CekCtx*) -> Value
      {
      (void)args;
      throw SchemeError("load: not implemented in this context");
      }, 1, 1);

   def(env, "null-environment", [](ArgVec args, CekCtx*) -> Value
      {
      (void)args;
      throw SchemeError("null-environment: not implemented");
      }, 1, 1);

   def(env, "scheme-report-environment", [](ArgVec args, CekCtx*) -> Value
      {
      (void)args;
      throw SchemeError("scheme-report-environment: not implemented");
      }, 1, 1);

   def(env, "environment", [](ArgVec, CekCtx*) -> Value
      { throw SchemeError("environment: not implemented in this context"); }, 0, -1);

   def(env, "syntax-expand", [](ArgVec, CekCtx*) -> Value
      { throw SchemeError("syntax-expand: not available at this level"); }, 1, 1);

   def(env, "gensym", [](ArgVec args, CekCtx*) -> Value
      {
      std::string prefix = (args.size() >= 1 && is_string(args[0])) ? as_string(args[0])->data : "g";
      static std::atomic<int64_t> counter{0};
      std::string name = prefix + std::to_string(counter.fetch_add(1));
      return make_symbol(name);
      }, 0, 1);
   }

// ── install_primitives ────────────────────────────────────────────────────────
void install_primitives(Environment* env)
   {
   // Initialize current port parameters
   SchemePort* stdin_p  = make_stdio_port(true,  stdin,  "<stdin>");
   SchemePort* stdout_p = make_stdio_port(false, stdout, "<stdout>");
   SchemePort* stderr_p = make_stdio_port(false, stderr, "<stderr>");

   g_current_input_port  = make_parameter_val(gc_alloc_parameter(make_port(stdin_p),  make_unspecified()));
   g_current_output_port = make_parameter_val(gc_alloc_parameter(make_port(stdout_p), make_unspecified()));
   g_current_error_port  = make_parameter_val(gc_alloc_parameter(make_port(stderr_p), make_unspecified()));

   register_arithmetic(env);
   register_comparison(env);
   register_predicates(env);
   register_equality(env);
   register_pairs(env);
   register_strings(env);
   register_chars(env);
   register_vectors(env);
   register_bytevectors(env);
   register_ports(env);
   register_lazy(env);
   register_control_stubs(env);
   register_meta(env);

   }
