// AST.cpp -- cons-cell AST and source-position infrastructure.
// Direct port of pyscheme/AST.py.

#include "AST.h"
#include "gc.h"
#include <memory>
#include <string>
#include <cstdlib>
#include <cinttypes>
#include <cstdio>

// ── Symbol intern pool ────────────────────────────────────────────────────────
// Port of AST.py _SYMBOL_POOL / _SYMBOL_NAMES.

static std::vector<std::string> g_symbol_names;
static std::unordered_map<std::string, uint32_t> g_symbol_pool;

uint32_t intern_symbol(const std::string& name)
   {
   auto it = g_symbol_pool.find(name);
   if (it != g_symbol_pool.end())
      return it->second;
   uint32_t nid = static_cast<uint32_t>(g_symbol_names.size());
   g_symbol_names.push_back(name);
   g_symbol_pool[name] = nid;
   return nid;
   }

std::string symbol_name(uint32_t sid)
   {
   return g_symbol_names[sid];
   }

const std::string GENSYM_PREFIX = "\x01h.";

std::string gensym_display_name(const std::string& name)
   {
   // Strip the hygiene gensym prefix for display / error messages:
   // \x01h.BASE.DIGITS -> BASE.  A non-gensym name is returned unchanged.
   if (name.compare(0, GENSYM_PREFIX.size(), GENSYM_PREFIX) != 0)
      return name;
   std::string rest = name.substr(GENSYM_PREFIX.size());
   size_t dot = rest.rfind('.');
   if (dot != std::string::npos)
      {
      std::string tail = rest.substr(dot + 1);
      bool all_digits = !tail.empty();
      for (char c : tail)
         if (c < '0' || c > '9')
            {
            all_digits = false;
            break;
            }
      if (all_digits)
         return rest.substr(0, dot);
      }
   return rest;
   }

// ── Singletons ────────────────────────────────────────────────────────────────
// Port of AST.py NIL_VALUE, VOID_VALUE, EOF_VALUE.

const Value NIL_VALUE{};
const Value VOID_VALUE{Value::Repr(VoidTag{})};
const Value EOF_VALUE{Value::Repr(EofTag{})};

// ── Primitive (Builtin) owner list ────────────────────────────────────────────
// Builtins are not GC-managed; held here for process lifetime.

static std::vector<std::unique_ptr<Builtin>> g_builtins;

// ── Integer / Real / Char pools ───────────────────────────────────────────────
// Pool objects are raw (not GC-registered) while dormant.  make_integer/real/char
// pops one, stamps in the value, calls gc_register_young, and returns a Value{p}.
// pool_return_* is called from gc_gen.cpp's free_object instead of delete.

static std::vector<SchemeInteger*> g_int_pool;
static std::vector<SchemeReal*> g_real_pool;
static std::vector<SchemeChar*> g_char_pool;

static const size_t POOL_INIT_SIZE = 512;

void init_value_pools()
   {
   g_int_pool.reserve(POOL_INIT_SIZE);
   g_real_pool.reserve(POOL_INIT_SIZE);
   g_char_pool.reserve(POOL_INIT_SIZE);
   for (size_t i = 0; i < POOL_INIT_SIZE; ++i)
      {
      g_int_pool.push_back(new SchemeInteger{});
      g_real_pool.push_back(new SchemeReal{});
      g_char_pool.push_back(new SchemeChar{});
      }
   }

void pool_return_integer(SchemeInteger* p)
   {
   g_int_pool.push_back(p);
   }
void pool_return_real(SchemeReal* p)
   {
   g_real_pool.push_back(p);
   }
void pool_return_char(SchemeChar* p)
   {
   g_char_pool.push_back(p);
   }

// ── Allocators ────────────────────────────────────────────────────────────────

Value alloc_cons(Value car_val, Value cdr_val, SourceInfo* src)
   {
   ConsCell* c = gc_alloc_cons();
   c->car = std::move(car_val);
   c->cdr = std::move(cdr_val);
   // Always clone src so each cons cell exclusively owns its SourceInfo.
   // Callers (parser, expander, syntax_rules) may pass either a freshly
   // allocated SourceInfo or a borrowed pointer from src_of(other_cell);
   // cloning gives uniform exclusive-ownership semantics for the GC sweep.
   c->src = src ? new SourceInfo(*src) : nullptr;
   return Value{Value::Repr(c)};
   }

Value make_nil(SourceInfo* src)
   {
   return Value{Value::Repr(NilAtom{src})};
   }

Value make_void()
   {
   return VOID_VALUE;
   }

Value make_boolean(bool b, SourceInfo* src)
   {
   return Value{Value::Repr(BoolAtom{b, src})};
   }

Value make_integer(int64_t n, SourceInfo* src)
   {
   SchemeInteger* p;
   if (g_int_pool.empty())
      {
      p = new SchemeInteger{};
      }
   else
      {
      p = g_int_pool.back();
      g_int_pool.pop_back();
      }
   p->value = n;
   p->src = src;
   gc_register_young(&p->header);
   return Value{Value::Repr(p)};
   }

Value make_real(double x, SourceInfo* src)
   {
   SchemeReal* p;
   if (g_real_pool.empty())
      {
      p = new SchemeReal{};
      }
   else
      {
      p = g_real_pool.back();
      g_real_pool.pop_back();
      }
   p->value = x;
   p->src = src;
   gc_register_young(&p->header);
   return Value{Value::Repr(p)};
   }

static int64_t scheme_gcd(int64_t a, int64_t b)
   {
   while (b)
      {
      int64_t t = a % b;
      a = b;
      b = t;
      }
   return a;
   }

Value make_rational(int64_t num, int64_t den, SourceInfo* src)
   {
   if (den < 0)
      {
      num = -num;
      den = -den;
      }
   int64_t n_abs = (num < 0) ? -num : num;
   int64_t g = scheme_gcd(n_abs, den);
   num /= g;
   den /= g;
   if (den == 1)
      return make_integer(num, src);
   SchemeRational* r = gc_alloc_rational(num, den);
   return Value{Value::Repr(r)};
   }

Value make_complex(double re, double im, SourceInfo* /*src*/)
   {
   SchemeComplex* z = gc_alloc_complex(re, im);
   return Value{Value::Repr(z)};
   }

Value make_exact_complex(Value re, Value im, SourceInfo* /*src*/)
   {
   ExactComplex* z = gc_alloc_exact_complex(std::move(re), std::move(im));
   return Value{Value::Repr(z)};
   }

Value make_character(char32_t c, SourceInfo* src)
   {
   SchemeChar* p;
   if (g_char_pool.empty())
      {
      p = new SchemeChar{};
      }
   else
      {
      p = g_char_pool.back();
      g_char_pool.pop_back();
      }
   p->value = c;
   p->src = src;
   gc_register_young(&p->header);
   return Value{Value::Repr(p)};
   }

Value make_string(const std::string& s, SourceInfo* src)
   {
   SchemeString* ss = gc_alloc_string(s);
   ss->src = src;
   return Value{Value::Repr(ss)};
   }

Value make_symbol(const std::string& name, SourceInfo* src)
   {
   return Value{Value::Repr(SymbolAtom{intern_symbol(name), src})};
   }

Value make_symbol_id(uint32_t sid, SourceInfo* src)
   {
   return Value{Value::Repr(SymbolAtom{sid, src})};
   }

Value make_closure(std::vector<uint32_t> params, Value body,
                   Environment* env, uint32_t rest_name_id,
                   std::string docstring)
   {
   SchemeClosure* cl = gc_alloc_closure();
   cl->params = std::move(params);
   cl->body = std::move(body);
   cl->env = env;
   cl->rest_name_id = rest_name_id;
   cl->docstring = std::move(docstring);
   return Value{Value::Repr(cl)};
   }

// Port of AST.py _PRIMITIVE_KIND_BY_NAME: maps the special-primitive
// names to their integer kind so make_primitive can stamp each Builtin.
static const std::unordered_map<std::string, int> g_primitive_kind_by_name = {
   {"call-with-current-continuation", PRIM_CALL_CC},
   {"call/cc",                        PRIM_CALL_CC},
   {"apply",                          PRIM_APPLY},
   {"call-with-values",               PRIM_CALL_WITH_VALUES},
   {"force",                          PRIM_FORCE},
   {"make-parameter",                 PRIM_MAKE_PARAMETER},
   {"with-exception-handler",         PRIM_WITH_EXCEPTION_HANDLER},
   {"%guard-eval",                    PRIM_GUARD_EVAL},
   {"raise",                          PRIM_RAISE},
   {"raise-continuable",              PRIM_RAISE_CONTINUABLE},
   {"eval",                           PRIM_EVAL},
   {"error",                          PRIM_ERROR},
   {"%with-parameters",               PRIM_WITH_PARAMETERS},
   {"dynamic-wind",                   PRIM_DYNAMIC_WIND},
   {"%continuation-depth",            PRIM_CONTINUATION_DEPTH},
   {"map",                            PRIM_MAP},
   {"for-each",                       PRIM_FOR_EACH},
   {"filter",                         PRIM_FILTER},
   {"vector-map",                     PRIM_VECTOR_MAP},
   {"vector-for-each",                PRIM_VECTOR_FOR_EACH},
   {"string-map",                     PRIM_STRING_MAP},
   {"string-for-each",                PRIM_STRING_FOR_EACH},
   {"member",                         PRIM_MEMBER},
   {"assoc",                          PRIM_ASSOC},
   // Port runners: open/validate then ride the dynamic-wind machinery with a
   // native after-thunk (close port; with-* also restore a current-port param)
   // -- see port_runner_setup / PRIM_PORT_RUNNER in the evaluator.
   {"call-with-port",                 PRIM_PORT_RUNNER},
   {"call-with-input-file",           PRIM_PORT_RUNNER},
   {"call-with-output-file",          PRIM_PORT_RUNNER},
   {"with-input-from-file",           PRIM_PORT_RUNNER},
   {"with-output-to-file",            PRIM_PORT_RUNNER},
   {"with-input-from-string",         PRIM_PORT_RUNNER},
   // load: read + parse the file natively, then evaluate its top-level forms on
   // the K stack via FRAME_EVAL_FORMS -- see load_setup / PRIM_LOAD.
   {"load",                           PRIM_LOAD},
   };

Value make_primitive(const std::string& name, BuiltinFn fn)
   {
   auto b = std::make_unique<Builtin>(name, std::move(fn));
   auto it = g_primitive_kind_by_name.find(name);
   b->kind = (it != g_primitive_kind_by_name.end()) ? it->second : PRIM_ORDINARY;
   Builtin* ptr = b.get();
   g_builtins.push_back(std::move(b));
   return Value{Value::Repr(ptr)};
   }

Value make_native_closure(const std::string& name, NativeFn fn,
                          std::vector<Value> captures)
   {
   NativeClosure* nc = gc_alloc_native_closure();
   nc->name = name;
   nc->fn = fn;
   nc->captures = std::move(captures);
   return Value{Value::Repr(nc)};
   }

Value make_case_closure(std::vector<CaseClosure::Clause> clauses,
                        Environment* env, std::string docstring)
   {
   CaseClosure* cc = gc_alloc_case_closure();
   cc->clauses = std::move(clauses);
   cc->env = env;
   cc->docstring = std::move(docstring);
   return Value{Value::Repr(cc)};
   }

Value make_promise_lazy(Value thunk, bool iterative)
   {
   Promise* p = gc_alloc_promise(std::move(thunk), false, iterative);
   return Value{Value::Repr(p)};
   }

Value make_promise_done(Value val)
   {
   Promise* p = gc_alloc_promise(std::move(val), true);
   return Value{Value::Repr(p)};
   }

Value make_multi_values(std::vector<Value> vals, SourceInfo* src)
   {
   MultiValues* mv = gc_alloc_multi_values();
   mv->values = std::move(vals);
   mv->src = src ? new SourceInfo(*src) : nullptr;
   return Value{Value::Repr(mv)};
   }

Value make_record_type(const std::string& name,
                       std::vector<uint32_t> field_name_ids)
   {
   RecordType* rt = gc_alloc_record_type();
   rt->name = name;
   rt->field_name_ids = std::move(field_name_ids);
   return Value{Value::Repr(rt)};
   }

Value make_record(RecordType* rt, std::vector<Value> field_values)
   {
   Record* rec = gc_alloc_record();
   rec->record_type = rt;
   rec->field_values = std::move(field_values);
   return Value{Value::Repr(rec)};
   }

Value make_parameter(Value val, Value converter)
   {
   Parameter* p = gc_alloc_parameter(std::move(val), std::move(converter));
   return Value{Value::Repr(p)};
   }

Value make_error_object(const std::string& message,
                        std::vector<Value> irritants)
   {
   ErrorObject* e = gc_alloc_error_object(message, std::move(irritants), 0);
   return Value{Value::Repr(e)};
   }

Value make_file_error_object(const std::string& message,
                             std::vector<Value> irritants)
   {
   ErrorObject* e = gc_alloc_error_object(message, std::move(irritants), 1);
   return Value{Value::Repr(e)};
   }

Value make_read_error_object(const std::string& message,
                             std::vector<Value> irritants)
   {
   ErrorObject* e = gc_alloc_error_object(message, std::move(irritants), 2);
   return Value{Value::Repr(e)};
   }

Value make_continuation(void* frames_ptr,
                        std::vector<WindFrame> wind_snapshot,
                        std::vector<Value> handler_snapshot,
                        std::vector<Value> shadow_snapshot)
   {
   Continuation* k = gc_alloc_continuation();
   k->frames_ptr = frames_ptr;
   k->wind_snapshot = std::move(wind_snapshot);
   k->handler_snapshot = std::move(handler_snapshot);
   k->shadow_snapshot = std::move(shadow_snapshot);
   return Value{Value::Repr(k)};
   }

Value make_syntax_transformer(
    const std::string& name,
    std::vector<uint32_t> literals,
    uint32_t ellipsis_id,
    std::vector<SyntaxTransformer::Rule> rules,
    std::unordered_map<uint32_t, uint32_t> free_id_map,
    std::unordered_set<uint32_t> intro_names,
    std::unordered_set<uint32_t> hygienic_intro_names)
   {
   SyntaxTransformer* st = gc_alloc_syntax_transformer();
   st->name = name;
   st->literals = std::move(literals);
   st->ellipsis_id = ellipsis_id;
   st->rules = std::move(rules);
   st->free_id_map = std::move(free_id_map);
   st->intro_names = std::move(intro_names);
   st->hygienic_intro_names = std::move(hygienic_intro_names);
   return Value{Value::Repr(st)};
   }

Value make_environment(Environment* env)
   {
   return Value{Value::Repr(gc_alloc_env_box(env))};
   }

Value make_record_accessor(RecordType* rt, int index, const std::string& name)
   {
   RecordAccessor* ra = gc_alloc_record_accessor(rt, index, name);
   return Value{Value::Repr(ra)};
   }

Value make_record_mutator(RecordType* rt, int index, const std::string& name)
   {
   RecordMutator* rm = gc_alloc_record_mutator(rt, index, name);
   return Value{Value::Repr(rm)};
   }

Value make_vector(std::vector<Value> items)
   {
   size_t n = items.size();
   SchemeVector* v = gc_alloc_vector(n);
   v->elements = std::move(items);
   return Value{Value::Repr(v)};
   }

Value make_bytevector(std::vector<uint8_t> items)
   {
   size_t n = items.size();
   SchemeBytevector* bv = gc_alloc_bytevector(n);
   bv->data = std::move(items);
   return Value{Value::Repr(bv)};
   }

Value make_port(bool is_input, bool is_text, const std::string& name)
   {
   Port* p = gc_alloc_port(is_input, is_text, name);
   return Value{Value::Repr(p)};
   }

Value make_eof()
   {
   return EOF_VALUE;
   }

// ── Bignum allocators ─────────────────────────────────────────────────────────

static SchemeBignum* gc_alloc_bignum()
   {
   auto* b = new SchemeBignum{};
   mpz_init(&b->value);
   gc_register_young(&b->header);
   return b;
   }

Value make_bignum_si(int64_t n, SourceInfo*)
   {
   auto* b = gc_alloc_bignum();
   // mpz_set_si takes a long; on MSVC x64 long is 32-bit, so split hi/lo.
   if (n >= INT32_MIN && n <= INT32_MAX)
      {
      mpz_set_si(&b->value, static_cast<long>(n));
      }
   else
      {
      // Build from string to handle values outside long range.
      char buf[24];
      if (n < 0)
         {
         std::snprintf(buf, sizeof(buf), "-%" PRIu64, static_cast<uint64_t>(-n));
         }
      else
         {
         std::snprintf(buf, sizeof(buf), "%" PRIu64, static_cast<uint64_t>(n));
         }
      mpz_set_str(&b->value, buf, 10);
      }
   return Value{Value::Repr(b)};
   }

Value make_bignum_str(const char* s, int base, SourceInfo*)
   {
   auto* b = gc_alloc_bignum();
   mpz_set_str(&b->value, s, base);
   return Value{Value::Repr(b)};
   }

Value make_bignum_copy(const __mpz_struct* z, SourceInfo*)
   {
   auto* b = gc_alloc_bignum();
   mpz_set(&b->value, z);
   return Value{Value::Repr(b)};
   }

const __mpz_struct* as_bignum(const Value& val)
   {
   return &std::get<SchemeBignum*>(val.repr)->value;
   }

std::string bignum_to_string(const Value& val, int base)
   {
   const __mpz_struct* z = as_bignum(val);
   char* s = mpz_get_str(nullptr, base, z);
   std::string result(s);
   free(s);
   return result;
   }

// ── Predicates ────────────────────────────────────────────────────────────────

bool is_cons(const Value& val)
   {
   return std::holds_alternative<ConsCell*>(val.repr);
   }
bool is_nil(const Value& val)
   {
   return std::holds_alternative<NilAtom>(val.repr);
   }
bool is_void(const Value& val)
   {
   return std::holds_alternative<VoidTag>(val.repr);
   }
bool is_boolean(const Value& val)
   {
   return std::holds_alternative<BoolAtom>(val.repr);
   }
bool is_integer(const Value& val)
   {
   return std::holds_alternative<SchemeInteger*>(val.repr);
   }
bool is_real(const Value& val)
   {
   return std::holds_alternative<SchemeReal*>(val.repr);
   }
bool is_rational(const Value& val)
   {
   return std::holds_alternative<SchemeRational*>(val.repr);
   }
bool is_complex(const Value& val)
   {
   return std::holds_alternative<SchemeComplex*>(val.repr);
   }
bool is_exact_complex(const Value& val)
   {
   return std::holds_alternative<ExactComplex*>(val.repr);
   }
bool is_character(const Value& val)
   {
   return std::holds_alternative<SchemeChar*>(val.repr);
   }
bool is_string(const Value& val)
   {
   return std::holds_alternative<SchemeString*>(val.repr);
   }
bool is_symbol(const Value& val)
   {
   return std::holds_alternative<SymbolAtom>(val.repr);
   }
bool is_closure(const Value& val)
   {
   return std::holds_alternative<SchemeClosure*>(val.repr);
   }
bool is_primitive(const Value& val)
   {
   return std::holds_alternative<Builtin*>(val.repr);
   }
bool is_case_closure(const Value& val)
   {
   return std::holds_alternative<CaseClosure*>(val.repr);
   }
bool is_promise(const Value& val)
   {
   return std::holds_alternative<Promise*>(val.repr);
   }
bool is_multi_values(const Value& val)
   {
   return std::holds_alternative<MultiValues*>(val.repr);
   }
bool is_record_type(const Value& val)
   {
   return std::holds_alternative<RecordType*>(val.repr);
   }
bool is_record(const Value& val)
   {
   return std::holds_alternative<Record*>(val.repr);
   }
bool is_parameter(const Value& val)
   {
   return std::holds_alternative<Parameter*>(val.repr);
   }
bool is_error_object(const Value& val)
   {
   return std::holds_alternative<ErrorObject*>(val.repr);
   }
bool is_continuation(const Value& val)
   {
   return std::holds_alternative<Continuation*>(val.repr);
   }
bool is_syntax_transformer(const Value& val)
   {
   return std::holds_alternative<SyntaxTransformer*>(val.repr);
   }
bool is_environment(const Value& val)
   {
   return std::holds_alternative<EnvBox*>(val.repr);
   }
bool is_record_accessor(const Value& val)
   {
   return std::holds_alternative<RecordAccessor*>(val.repr);
   }
bool is_record_mutator(const Value& val)
   {
   return std::holds_alternative<RecordMutator*>(val.repr);
   }
bool is_vector(const Value& val)
   {
   return std::holds_alternative<SchemeVector*>(val.repr);
   }
bool is_bytevector(const Value& val)
   {
   return std::holds_alternative<SchemeBytevector*>(val.repr);
   }
bool is_port(const Value& val)
   {
   return std::holds_alternative<Port*>(val.repr);
   }
bool is_eof(const Value& val)
   {
   return std::holds_alternative<EofTag>(val.repr);
   }

bool is_file_error_object(const Value& val)
   {
   auto* pp = std::get_if<ErrorObject*>(&val.repr);
   return pp && *pp && (*pp)->kind == 1;
   }

bool is_read_error_object(const Value& val)
   {
   auto* pp = std::get_if<ErrorObject*>(&val.repr);
   return pp && *pp && (*pp)->kind == 2;
   }

bool is_truthy(const Value& val)
   {
   if (auto* pp = std::get_if<BoolAtom>(&val.repr))
      return pp->value;
   return true; // everything except #f is truthy
   }

bool is_number(const Value& val)
   {
   return is_integer(val) || is_bignum(val) || is_real(val) || is_rational(val) ||
          is_complex(val) || is_exact_complex(val);
   }

bool is_bignum(const Value& val)
   {
   return std::holds_alternative<SchemeBignum*>(val.repr);
   }

bool is_procedure(const Value& val)
   {
   return is_closure(val) || is_case_closure(val) || is_primitive(val) ||
          is_continuation(val) || is_parameter(val) ||
          is_record_accessor(val) || is_record_mutator(val);
   }

// ── Accessors ─────────────────────────────────────────────────────────────────

bool as_boolean(const Value& val)
   {
   return std::get<BoolAtom>(val.repr).value;
   }

int64_t as_integer(const Value& val)
   {
   return std::get<SchemeInteger*>(val.repr)->value;
   }
SourceInfo* integer_src(const Value& val)
   {
   return std::get<SchemeInteger*>(val.repr)->src;
   }

double as_real(const Value& val)
   {
   return std::get<SchemeReal*>(val.repr)->value;
   }

int64_t as_rational_num(const Value& val)
   {
   return std::get<SchemeRational*>(val.repr)->num;
   }

int64_t as_rational_den(const Value& val)
   {
   return std::get<SchemeRational*>(val.repr)->den;
   }

double as_complex_real(const Value& val)
   {
   return std::get<SchemeComplex*>(val.repr)->real;
   }

double as_complex_imag(const Value& val)
   {
   return std::get<SchemeComplex*>(val.repr)->imag;
   }

Value as_exact_complex_real(const Value& val)
   {
   return std::get<ExactComplex*>(val.repr)->re;
   }

Value as_exact_complex_imag(const Value& val)
   {
   return std::get<ExactComplex*>(val.repr)->im;
   }

char32_t as_character(const Value& val)
   {
   return std::get<SchemeChar*>(val.repr)->value;
   }
SourceInfo* character_src(const Value& val)
   {
   return std::get<SchemeChar*>(val.repr)->src;
   }

const std::string& as_string(const Value& val)
   {
   return std::get<SchemeString*>(val.repr)->data;
   }

std::string& as_string_mut(Value& val)
   {
   return std::get<SchemeString*>(val.repr)->data;
   }

std::string as_symbol(const Value& val)
   {
   return g_symbol_names[std::get<SymbolAtom>(val.repr).id];
   }

uint32_t as_symbol_id(const Value& val)
   {
   return std::get<SymbolAtom>(val.repr).id;
   }

const std::vector<uint32_t>& as_closure_params(const Value& val)
   {
   return std::get<SchemeClosure*>(val.repr)->params;
   }

Value as_closure_body(const Value& val)
   {
   return std::get<SchemeClosure*>(val.repr)->body;
   }

Environment* as_closure_env(const Value& val)
   {
   return std::get<SchemeClosure*>(val.repr)->env;
   }

uint32_t as_closure_rest_name(const Value& val)
   {
   return std::get<SchemeClosure*>(val.repr)->rest_name_id;
   }

const std::string& as_closure_docstring(const Value& val)
   {
   return std::get<SchemeClosure*>(val.repr)->docstring;
   }

const std::string& as_primitive_name(const Value& val)
   {
   return std::get<Builtin*>(val.repr)->name;
   }

const BuiltinFn& as_primitive_fn(const Value& val)
   {
   return std::get<Builtin*>(val.repr)->fn;
   }

int as_primitive_kind(const Value& val)
   {
   return std::get<Builtin*>(val.repr)->kind;
   }

bool is_native_closure(const Value& val)
   {
   return std::holds_alternative<NativeClosure*>(val.repr);
   }

NativeFn as_native_closure_fn(const Value& val)
   {
   return std::get<NativeClosure*>(val.repr)->fn;
   }

std::vector<Value>& as_native_closure_captures(const Value& val)
   {
   return std::get<NativeClosure*>(val.repr)->captures;
   }

const std::string& as_native_closure_name(const Value& val)
   {
   return std::get<NativeClosure*>(val.repr)->name;
   }

const std::vector<CaseClosure::Clause>& as_case_closure_clauses(const Value& val)
   {
   return std::get<CaseClosure*>(val.repr)->clauses;
   }

Environment* as_case_closure_env(const Value& val)
   {
   return std::get<CaseClosure*>(val.repr)->env;
   }

const std::string& as_case_closure_docstring(const Value& val)
   {
   return std::get<CaseClosure*>(val.repr)->docstring;
   }

bool as_promise_is_done(const Value& val)
   {
   return std::get<Promise*>(val.repr)->is_done;
   }

bool as_promise_is_iterative(const Value& val)
   {
   return std::get<Promise*>(val.repr)->iterative;
   }

Value as_promise_payload(const Value& val)
   {
   return std::get<Promise*>(val.repr)->payload;
   }

void promise_resolve(Value& promise_val, Value result)
   {
   Promise* p = std::get<Promise*>(promise_val.repr);
   p->is_done = true;
   p->payload = std::move(result);
   gc_write_barrier(&p->header, gc_value_header(p->payload));
   }

void promise_become(Value& dst, const Value& src_val)
   {
   Promise* d = std::get<Promise*>(dst.repr);
   const Promise* s = std::get<Promise*>(src_val.repr);
   d->is_done = s->is_done;
   d->iterative = s->iterative; // inherit chase/no-chase of the link we became
   d->payload = s->payload;
   gc_write_barrier(&d->header, gc_value_header(d->payload));
   }

const std::vector<Value>& as_multi_values_list(const Value& val)
   {
   return std::get<MultiValues*>(val.repr)->values;
   }

RecordType* as_record_type_obj(const Value& val)
   {
   return std::get<RecordType*>(val.repr);
   }

const std::string& as_record_type_name(const Value& val)
   {
   return std::get<RecordType*>(val.repr)->name;
   }

const std::vector<uint32_t>& as_record_type_field_names(const Value& val)
   {
   return std::get<RecordType*>(val.repr)->field_name_ids;
   }

RecordType* as_record_type(const Value& record_val)
   {
   return std::get<Record*>(record_val.repr)->record_type;
   }

std::vector<Value>& as_record_fields(Value& val)
   {
   return std::get<Record*>(val.repr)->field_values;
   }

const std::vector<Value>& as_record_fields_const(const Value& val)
   {
   return std::get<Record*>(val.repr)->field_values;
   }

Value as_parameter_value(const Value& val)
   {
   return std::get<Parameter*>(val.repr)->value;
   }

Value as_parameter_converter(const Value& val)
   {
   return std::get<Parameter*>(val.repr)->converter;
   }

void set_parameter_value(Value& val, Value newval)
   {
   Parameter* p = std::get<Parameter*>(val.repr);
   p->value = std::move(newval);
   gc_write_barrier(&p->header, gc_value_header(p->value));
   }

const std::string& as_error_object_message(const Value& val)
   {
   return std::get<ErrorObject*>(val.repr)->message;
   }

const std::vector<Value>& as_error_object_irritants(const Value& val)
   {
   return std::get<ErrorObject*>(val.repr)->irritants;
   }

int as_error_object_kind(const Value& val)
   {
   return std::get<ErrorObject*>(val.repr)->kind;
   }

void* as_continuation_frames(const Value& val)
   {
   return std::get<Continuation*>(val.repr)->frames_ptr;
   }

const std::vector<WindFrame>& as_continuation_wind(const Value& val)
   {
   return std::get<Continuation*>(val.repr)->wind_snapshot;
   }

const std::vector<Value>& as_continuation_handlers(const Value& val)
   {
   return std::get<Continuation*>(val.repr)->handler_snapshot;
   }

const std::vector<Value>& as_continuation_shadow(const Value& val)
   {
   return std::get<Continuation*>(val.repr)->shadow_snapshot;
   }

const std::string& as_syntax_transformer_name(const Value& val)
   {
   return std::get<SyntaxTransformer*>(val.repr)->name;
   }

const std::vector<uint32_t>& as_syntax_transformer_literals(const Value& val)
   {
   return std::get<SyntaxTransformer*>(val.repr)->literals;
   }

uint32_t as_syntax_transformer_ellipsis(const Value& val)
   {
   return std::get<SyntaxTransformer*>(val.repr)->ellipsis_id;
   }

const std::vector<SyntaxTransformer::Rule>& as_syntax_transformer_rules(const Value& val)
   {
   return std::get<SyntaxTransformer*>(val.repr)->rules;
   }

const std::unordered_map<uint32_t, uint32_t>& as_syntax_transformer_free_id_map(const Value& val)
   {
   return std::get<SyntaxTransformer*>(val.repr)->free_id_map;
   }

const std::unordered_set<uint32_t>& as_syntax_transformer_intro_names(const Value& val)
   {
   return std::get<SyntaxTransformer*>(val.repr)->intro_names;
   }

const std::unordered_set<uint32_t>& as_syntax_transformer_hygienic_intro_names(const Value& val)
   {
   return std::get<SyntaxTransformer*>(val.repr)->hygienic_intro_names;
   }

Environment* as_environment(const Value& val)
   {
   return std::get<EnvBox*>(val.repr)->env;
   }

RecordType* as_record_accessor_type(const Value& val)
   {
   return std::get<RecordAccessor*>(val.repr)->record_type;
   }

int as_record_accessor_index(const Value& val)
   {
   return std::get<RecordAccessor*>(val.repr)->index;
   }

const std::string& as_record_accessor_name(const Value& val)
   {
   return std::get<RecordAccessor*>(val.repr)->name;
   }

RecordType* as_record_mutator_type(const Value& val)
   {
   return std::get<RecordMutator*>(val.repr)->record_type;
   }

int as_record_mutator_index(const Value& val)
   {
   return std::get<RecordMutator*>(val.repr)->index;
   }

const std::string& as_record_mutator_name(const Value& val)
   {
   return std::get<RecordMutator*>(val.repr)->name;
   }

std::vector<Value>& as_vector_items(Value& val)
   {
   return std::get<SchemeVector*>(val.repr)->elements;
   }

const std::vector<Value>& as_vector_items_const(const Value& val)
   {
   return std::get<SchemeVector*>(val.repr)->elements;
   }

std::vector<uint8_t>& as_bytevector_items(Value& val)
   {
   return std::get<SchemeBytevector*>(val.repr)->data;
   }

const std::vector<uint8_t>& as_bytevector_items_const(const Value& val)
   {
   return std::get<SchemeBytevector*>(val.repr)->data;
   }

Port* as_port(const Value& val)
   {
   return std::get<Port*>(val.repr);
   }

// ── Immutability ──────────────────────────────────────────────────────────────
// Port of AST.py is_immutable / mark_literal_immutable.

bool is_immutable(const Value& val)
   {
   if (is_cons(val))
      return std::get<ConsCell*>(val.repr)->immutable;
   if (is_string(val))
      return std::get<SchemeString*>(val.repr)->immutable;
   if (is_vector(val))
      return std::get<SchemeVector*>(val.repr)->immutable;
   if (is_bytevector(val))
      return std::get<SchemeBytevector*>(val.repr)->immutable;
   return false;
   }

void mark_literal_immutable(const Value& val)
   {
   // Iterative (explicit worklist) so a deeply-nested or very long literal does
   // not overflow the C stack.  The immutable flag doubles as the visited marker,
   // bounding cycles and re-shared structure.  No allocation here, so the Values
   // held in the worklist need no GC rooting.
   std::vector<Value> stack;
   stack.push_back(val);
   while (!stack.empty())
      {
      Value v = stack.back();
      stack.pop_back();
      if (is_cons(v))
         {
         ConsCell* c = std::get<ConsCell*>(v.repr);
         if (c->immutable)
            continue;
         c->immutable = true;
         stack.push_back(c->car);
         stack.push_back(c->cdr);
         }
      else if (is_string(v))
         {
         std::get<SchemeString*>(v.repr)->immutable = true;
         }
      else if (is_vector(v))
         {
         SchemeVector* sv = std::get<SchemeVector*>(v.repr);
         if (sv->immutable)
            continue;
         sv->immutable = true;
         for (const Value& item : sv->elements)
            stack.push_back(item);
         }
      else if (is_bytevector(v))
         {
         std::get<SchemeBytevector*>(v.repr)->immutable = true;
         }
      }
   }

// ── Pair operations ───────────────────────────────────────────────────────────

Value car(const Value& val)
   {
   return std::get<ConsCell*>(val.repr)->car;
   }

Value cdr(const Value& val)
   {
   return std::get<ConsCell*>(val.repr)->cdr;
   }

void set_car(Value& cons_val, Value new_car)
   {
   ConsCell* cell = std::get<ConsCell*>(cons_val.repr);
   cell->car = std::move(new_car);
   gc_write_barrier(&cell->header, gc_value_header(cell->car));
   }

void set_cdr(Value& cons_val, Value new_cdr)
   {
   ConsCell* cell = std::get<ConsCell*>(cons_val.repr);
   cell->cdr = std::move(new_cdr);
   gc_write_barrier(&cell->header, gc_value_header(cell->cdr));
   }

// ── Value equality ────────────────────────────────────────────────────────────
// Port of AST.py eqv_atom.

bool eqv_atom(const Value& a, const Value& b)
   {
      // Port of Python: if a is b: return True
      {
      auto get_ptr = [](const Value& v) -> const void*
      {
         return std::visit([](auto&& x) -> const void*
                           {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_pointer_v<T>) return static_cast<const void*>(x);
                return nullptr; }, v.repr);
      };
      const void* pa = get_ptr(a);
      const void* pb = get_ptr(b);
      if (pa && pb && pa == pb)
         return true;
      }
   if (is_symbol(a) && is_symbol(b))
      return as_symbol_id(a) == as_symbol_id(b);
   if (is_boolean(a) && is_boolean(b))
      return as_boolean(a) == as_boolean(b);
   if (is_nil(a) && is_nil(b))
      return true;
   if (is_integer(a) && is_integer(b))
      return as_integer(a) == as_integer(b);
   if (is_bignum(a) && is_bignum(b))
      return mpz_cmp(as_bignum(a), as_bignum(b)) == 0;
   // int64 vs bignum: bignums only arise from overflow, so always unequal.
   if ((is_integer(a) && is_bignum(b)) || (is_bignum(a) && is_integer(b)))
      return false;
   if (is_real(a) && is_real(b))
      return as_real(a) == as_real(b);
   if (is_rational(a) && is_rational(b))
      return as_rational_num(a) == as_rational_num(b) &&
             as_rational_den(a) == as_rational_den(b);
   if (is_complex(a) && is_complex(b))
      return as_complex_real(a) == as_complex_real(b) &&
             as_complex_imag(a) == as_complex_imag(b);
   if (is_exact_complex(a) && is_exact_complex(b))
      return eqv_atom(as_exact_complex_real(a), as_exact_complex_real(b)) &&
             eqv_atom(as_exact_complex_imag(a), as_exact_complex_imag(b));
   if (is_character(a) && is_character(b))
      return as_character(a) == as_character(b);
   return false;
   }

// ── List construction helper ──────────────────────────────────────────────────
// Port of AST.py list_from_items.

Value list_from_items(const std::vector<Value>& items, SourceInfo* src)
   {
   Value result = NIL_VALUE;
   for (int i = static_cast<int>(items.size()) - 1; i >= 0; --i)
      result = alloc_cons(items[i], result, src);
   return result;
   }

// ── Source info extraction ────────────────────────────────────────────────────
// Port of AST.py src_of.
// Option A: all atom types carry SourceInfo* so this matches Python exactly.

SourceInfo* src_of(const Value& val)
   {
   if (auto* pp = std::get_if<NilAtom>(&val.repr))
      return pp->src;
   if (auto* pp = std::get_if<BoolAtom>(&val.repr))
      return pp->src;
   if (auto* pp = std::get_if<SchemeInteger*>(&val.repr))
      return *pp ? (*pp)->src : nullptr;
   if (auto* pp = std::get_if<SchemeReal*>(&val.repr))
      return *pp ? (*pp)->src : nullptr;
   if (auto* pp = std::get_if<SchemeChar*>(&val.repr))
      return *pp ? (*pp)->src : nullptr;
   if (auto* pp = std::get_if<SymbolAtom>(&val.repr))
      return pp->src;
   if (auto* pp = std::get_if<ConsCell*>(&val.repr))
      return (*pp)->src;
   if (auto* pp = std::get_if<SchemeString*>(&val.repr))
      return (*pp)->src;
   if (auto* pp = std::get_if<MultiValues*>(&val.repr))
      return (*pp)->src;
   return nullptr;
   }

// ── Diagnostics ───────────────────────────────────────────────────────────────
// Port of AST.py format_with_caret.

std::string format_with_caret(const std::string& msg, SourceInfo* src)
   {
   if (!src)
      return msg;
   std::string prefix;
   if (!src->filename.empty())
      prefix = '"' + src->filename + "\" line " + std::to_string(src->line) +
               ", col " + std::to_string(src->col) + ": " + msg;
   else
      prefix = "line " + std::to_string(src->line) +
               ", col " + std::to_string(src->col) + ": " + msg;
   if (src->source_line.empty())
      return prefix;
   int pad = src->col - 1;
   std::string caret(static_cast<size_t>(pad > 0 ? pad : 0), ' ');
   caret += '^';
   return prefix + '\n' + src->source_line + '\n' + caret;
   }

// ── GC support ────────────────────────────────────────────────────────────────

// The GC-managed Value-variant pointer types, listed once.  gc_value_header and
// gc_forward_value both iterate this single list (EnvBox is handled separately
// below: its type is incomplete in this TU, so it uses a raw GcHeader cast).
// Adding a new boxed Value type means adding one entry here.
#define GC_VALUE_PTR_TYPES(X)                                              \
   X(ConsCell) X(SchemeString) X(SchemeClosure) X(CaseClosure) X(Promise) \
   X(MultiValues) X(Record) X(RecordType) X(Parameter) X(ErrorObject)     \
   X(Continuation) X(SyntaxTransformer) X(SchemeVector) X(SchemeBytevector) \
   X(Port) X(SchemeComplex) X(ExactComplex) X(SchemeRational) X(SchemeBignum) \
   X(SchemeInteger) X(SchemeReal) X(SchemeChar) X(RecordAccessor)         \
   X(RecordMutator) X(NativeClosure)

// Expands to the GcHeader* for a named GC-managed pointer type, or nullptr.
#define HDR_CASE(Type)                           \
   if (auto* pp = std::get_if<Type*>(&val.repr)) \
      return *pp ? &(*pp)->header : nullptr;

GcHeader* gc_value_header(const Value& val)
   {
   GC_VALUE_PTR_TYPES(HDR_CASE)
   // EnvBox* is GC-managed; GcHeader is at offset 0 by invariant.
   if (auto* pp = std::get_if<EnvBox*>(&val.repr))
      return *pp ? reinterpret_cast<GcHeader*>(*pp) : nullptr;
   return nullptr;
   }

#undef HDR_CASE

// Expands to: if val holds a Type*, check its forwarding pointer and update val.
#define FWD_CASE(Type)                                                           \
   if (auto* pp = std::get_if<Type*>(&val.repr))                                 \
      {                                                                          \
      if (*pp && (*pp)->header.forward)                                          \
         val.repr = Value::Repr(reinterpret_cast<Type*>((*pp)->header.forward)); \
      return;                                                                    \
      }

void gc_forward_value(Value& val)
   {
   GC_VALUE_PTR_TYPES(FWD_CASE)
   // EnvBox* forwarding via raw GcHeader cast (type is incomplete here).
   if (auto* pp = std::get_if<EnvBox*>(&val.repr))
      {
      if (*pp)
         {
         GcHeader* hdr = reinterpret_cast<GcHeader*>(*pp);
         if (hdr->forward)
            val.repr = Value::Repr(reinterpret_cast<EnvBox*>(hdr->forward));
         }
      }
   }

#undef FWD_CASE
#undef GC_VALUE_PTR_TYPES
