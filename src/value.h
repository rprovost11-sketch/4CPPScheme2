#pragma once
// value.h — Value representation + GC object header types.
//
// Swappability contract:
//   All struct layouts and variant arms live in this file (variant implementation).
//   To swap to NaN-boxing or tagged-pointer, replace this file and value.cpp.
//   No other source file may access Value::repr, ConsCell fields, or any other
//   layout detail directly — use the public API functions declared below.
//
// Symbol representation:
//   Symbols are represented as SymbolId (uint32_t intern-pool index), not Symbol*.
//   Use intern_symbol(name) to obtain a SymbolId; symbol_name(sid) to recover name.
//   ID equality implies name equality; eq? on symbols is a single integer compare.

#include <cstdint>
#include <atomic>
#include "scheme_export.h"
#include "symbol.h"
#include "exceptions.h"
#include <variant>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <iosfwd>

// ── Sentinel for "no rest parameter" in closures, macros, and case-clauses ───
static constexpr uint32_t NO_REST_PARAM = 0xFFFFFFFFu;

// ── GC object type tags ───────────────────────────────────────────────────────
enum class GcType : uint8_t
   {
   Cons,
   String,
   Closure,
   Macro,
   SyntaxTransformer,
   Continuation,
   Vector,
   Bytevector,
   Environment,
   Promise,
   Parameter,
   Port,
   Complex,
   Rational,
   CaseClosure,
   MultiValues,
   Record,
   ExactComplex,
   ErrorObject,
   };

// ── GC intrusive header ───────────────────────────────────────────────────────
struct GcHeader
   {
   std::atomic<bool>    marked{false};
   std::atomic<uint8_t> gen{0};
   GcType    type;
   GcHeader* next    = nullptr;
   GcHeader* forward = nullptr;

   explicit GcHeader(GcType object_type) : type(object_type) {}

   GcHeader(const GcHeader&)            = delete;
   GcHeader& operator=(const GcHeader&) = delete;
   };

// ── Forward declarations for heap object types ───────────────────────────────
struct ConsCell;
struct SchemeString;
struct SchemeClosure;
struct SchemeMacro;
struct SchemeSyntaxTransformer;
struct SchemeContinuation;
struct SchemeVector;
struct SchemeBytevector;
struct SchemePromise;
struct SchemeParameter;
struct SchemePort;
struct SchemeComplex;
struct SchemeRational;
struct Environment;
struct SchemeCaseClosure;
struct SchemeMultiValues;
struct SchemeRecord;
struct SchemeExactComplex;
struct SchemeErrorObject;

// ── Non-GC record type descriptor ────────────────────────────────────────────
// One descriptor per define-record-type form; shared by identity across all
// instances of that type.  Static lifetime — not GC-managed.
struct SchemeRecordType
   {
   std::string              name;
   std::vector<std::string> field_names;

   SchemeRecordType(std::string type_name, std::vector<std::string> fields)
      : name(std::move(type_name)), field_names(std::move(fields))
      {}

   SchemeRecordType(const SchemeRecordType&) = delete;
   SchemeRecordType& operator=(const SchemeRecordType&) = delete;
   };

// ── Builtin procedure ─────────────────────────────────────────────────────────
struct Value;
struct Builtin;

// Sentinel types for immediate values
struct Unspecified {};
struct EofTag {};

// ── Symbol immediate ──────────────────────────────────────────────────────────
// Wraps a uint32_t intern-pool ID as a distinct variant arm.
struct SymbolId
   {
   uint32_t id;
   explicit SymbolId(uint32_t symbol_id) : id(symbol_id) {}
   };

// ── THE Value type ───────────────────────────────────────────────────────────
struct Value
   {
   using Repr = std::variant<
      std::monostate,            // nil / '()
      bool,                      // #t / #f
      int64_t,                   // fixnum (exact integer)
      double,                    // flonum (inexact real)
      char32_t,                  // character (Unicode codepoint; immediate)
      EofTag,                    // eof-object (singleton; immediate)
      SymbolId,                  // symbol (integer ID; immediate)
      ConsCell*,                 // pair (GC-managed)
      SchemeString*,             // string (GC-managed, mutable)
      SchemeClosure*,            // lambda closure (GC-managed)
      SchemeMacro*,              // define-macro transformer (GC-managed)
      SchemeSyntaxTransformer*,  // syntax-rules transformer (GC-managed)
      SchemeContinuation*,       // captured continuation (GC-managed)
      SchemeVector*,             // vector (GC-managed)
      SchemeBytevector*,         // bytevector (GC-managed)
      Builtin*,                  // primitive procedure (static lifetime)
      Environment*,              // first-class environment (GC-managed)
      Unspecified,               // unspecified return value
      SchemePromise*,            // lazy promise (GC-managed)
      SchemeParameter*,          // dynamic parameter (GC-managed)
      SchemePort*,               // I/O port (GC-managed)
      SchemeComplex*,            // inexact complex (GC-managed)
      SchemeRational*,           // exact rational (GC-managed)
      SchemeCaseClosure*,        // case-lambda closure (GC-managed)
      SchemeMultiValues*,        // values() return (GC-managed)
      SchemeRecord*,             // record instance (GC-managed)
      SchemeExactComplex*,       // exact complex (GC-managed)
      SchemeErrorObject*         // R7RS error-object (GC-managed)
   >;
   Repr repr;

   Value()                    : repr(std::monostate{}) {}
   explicit Value(Repr inner) : repr(std::move(inner)) {}
   };


// ── ArgVec: non-owning view of a contiguous Value array ──────────────────────
struct ArgVec
   {
   Value* data_ = nullptr;
   size_t size_ = 0;

   ArgVec() = default;
   ArgVec(Value* p, size_t n) : data_(p), size_(n) {}
   ArgVec(std::vector<Value>& v) : data_(v.data()), size_(v.size()) {}

   size_t       size()  const { return size_; }
   bool         empty() const { return size_ == 0; }

   Value&       operator[](size_t i)       { return data_[i]; }
   const Value& operator[](size_t i) const { return data_[i]; }

   Value&       back()       { return data_[size_ - 1]; }
   const Value& back() const { return data_[size_ - 1]; }

   Value*       begin()       { return data_; }
   Value*       end()         { return data_ + size_; }
   const Value* begin() const { return data_; }
   const Value* end()   const { return data_ + size_; }
   };


// ── ArgBuf: small-buffer optimised argument buffer (owning) ──────────────────
struct ArgBuf
   {
   static constexpr size_t INLINE_CAP = 6;
   size_t             size_ = 0;
   Value              buf_[INLINE_CAP];
   std::vector<Value> overflow_;

   ArgBuf() = default;
   ArgBuf(std::initializer_list<Value> init) { for (const Value& v : init) push_back(v); }
   ArgBuf(const ArgBuf&)            = delete;
   ArgBuf(ArgBuf&&)                 = default;
   ArgBuf& operator=(const ArgBuf&) = delete;
   ArgBuf& operator=(ArgBuf&&)      = default;

   size_t size()  const { return size_; }
   bool   empty() const { return size_ == 0; }

   Value*       data()       { return size_ <= INLINE_CAP ? buf_ : overflow_.data(); }
   const Value* data() const { return size_ <= INLINE_CAP ? buf_ : overflow_.data(); }

   operator ArgVec() { return {data(), size_}; }

   void push_back(const Value& v)
      {
      if (size_ < INLINE_CAP)
         {
         buf_[size_++] = v;
         }
      else if (size_ == INLINE_CAP)
         {
         overflow_.reserve(INLINE_CAP + 1);
         for (size_t i = 0; i < INLINE_CAP; ++i) overflow_.push_back(std::move(buf_[i]));
         overflow_.push_back(v);
         ++size_;
         }
      else
         {
         overflow_.push_back(v);
         ++size_;
         }
      }

   void reserve(size_t n) { if (n > INLINE_CAP) overflow_.reserve(n); }
   };


using BuiltinFn = std::function<Value(ArgVec)>;

struct Builtin
   {
   std::string name;
   BuiltinFn   fn;
   int         min_args = -1;
   int         max_args = -1;

   Builtin(std::string builtin_name, BuiltinFn builtin_fn)
      : name(std::move(builtin_name)), fn(std::move(builtin_fn))
      {}
   };


// ── Dynamic-wind frame ───────────────────────────────────────────────────────
struct WindFrame
   {
   Value before;
   Value after;
   };


// ── Heap object definitions ──────────────────────────────────────────────────
// Every struct begins with GcHeader.  Structs that embed Value fields must be
// defined AFTER Value (complete type required for by-value storage).

struct ConsCell
   {
   GcHeader header{GcType::Cons};
   Value    car{};
   Value    cdr{};
   };

struct SchemeString
   {
   GcHeader    header{GcType::String};
   std::string data;

   explicit SchemeString(std::string content) : data(std::move(content)) {}
   };

struct SchemeClosure
   {
   GcHeader              header{GcType::Closure};
   std::vector<uint32_t> params;       // positional parameter SIDs
   uint32_t              rest_param;   // NO_REST_PARAM if none
   Value                 body;
   Environment*          env;
   std::string           name;

   SchemeClosure() : rest_param(NO_REST_PARAM), env(nullptr) {}
   };

struct SchemeMacro
   {
   GcHeader              header{GcType::Macro};
   std::vector<uint32_t> params;
   uint32_t              rest_param;
   Value                 body;
   Environment*          env;
   std::string           name;

   SchemeMacro() : rest_param(NO_REST_PARAM), env(nullptr) {}
   };

struct SchemeSyntaxTransformer
   {
   GcHeader              header{GcType::SyntaxTransformer};
   std::string           name;
   std::vector<uint32_t> literals;   // literal symbol SIDs to match exactly
   uint32_t              ellipsis;   // ellipsis SID (default: intern("..."))
   struct Rule
      {
      Value pattern;
      Value tmpl;
      };
   std::vector<Rule> rules;
   Environment*      def_env;

   SchemeSyntaxTransformer() : ellipsis(0), def_env(nullptr) {}
   };

struct SchemeContinuation
   {
   GcHeader               header{GcType::Continuation};
   void*                  frames_ptr = nullptr;
   std::vector<WindFrame> wind_stack;
   std::vector<Value>     arg_stack_snapshot;

   SchemeContinuation()                                   = default;
   ~SchemeContinuation();
   SchemeContinuation(const SchemeContinuation&)          = delete;
   SchemeContinuation& operator=(const SchemeContinuation&) = delete;
   };

struct SchemeVector
   {
   GcHeader           header{GcType::Vector};
   std::vector<Value> elements;

   explicit SchemeVector(size_t n, Value fill = Value{}) : elements(n, fill) {}
   };

struct SchemeBytevector
   {
   GcHeader             header{GcType::Bytevector};
   std::vector<uint8_t> data;

   SchemeBytevector() = default;
   explicit SchemeBytevector(size_t n, uint8_t fill = 0) : data(n, fill) {}
   };

struct SchemePromise
   {
   GcHeader header{GcType::Promise};
   bool     forced = false;
   Value    val;
   };

struct SchemeParameter
   {
   GcHeader header{GcType::Parameter};
   Value    current;
   Value    converter;
   };

struct SchemeComplex
   {
   GcHeader header{GcType::Complex};
   double   real;
   double   imag;

   SchemeComplex(double r, double i) : real(r), imag(i) {}
   };

struct SchemeRational
   {
   GcHeader header{GcType::Rational};
   int64_t  num;
   int64_t  den;

   SchemeRational(int64_t n, int64_t d) : num(n), den(d) {}
   };

// ── New GC types added in CEKScheme ──────────────────────────────────────────

struct SchemeCaseClosure
   {
   GcHeader header{GcType::CaseClosure};
   struct Clause
      {
      std::vector<uint32_t> params;
      uint32_t              rest_param;   // NO_REST_PARAM if none
      Value                 body;
      };
   std::vector<Clause> clauses;
   Environment*        env;
   std::string         name;

   SchemeCaseClosure() : env(nullptr) {}
   };

struct SchemeMultiValues
   {
   GcHeader           header{GcType::MultiValues};
   std::vector<Value> values;
   };

struct SchemeRecord
   {
   GcHeader           header{GcType::Record};
   SchemeRecordType*  type;   // static lifetime, not GC-managed
   std::vector<Value> fields;

   SchemeRecord() : type(nullptr) {}
   };

struct SchemeExactComplex
   {
   GcHeader header{GcType::ExactComplex};
   Value    real;   // fixnum or rational
   Value    imag;   // fixnum or rational (invariant: non-zero)
   };

struct SchemeErrorObject
   {
   GcHeader    header{GcType::ErrorObject};
   std::string message;
   Value       irritants;   // cons-list of irritants
   int         kind;        // 0=generic  1=file-error  2=read-error

   SchemeErrorObject() : kind(0) {}
   };


// ── Public API ───────────────────────────────────────────────────────────────

// --- Type predicates ---
SCHEME_API bool is_nil(Value val);
SCHEME_API bool is_bool(Value val);
SCHEME_API bool is_fixnum(Value val);
SCHEME_API bool is_flonum(Value val);
SCHEME_API bool is_number(Value val);
SCHEME_API bool is_symbol(Value val);
SCHEME_API bool is_cons(Value val);
SCHEME_API bool is_string(Value val);
SCHEME_API bool is_closure(Value val);
SCHEME_API bool is_case_closure(Value val);
SCHEME_API bool is_macro(Value val);
SCHEME_API bool is_syntax_transformer(Value val);
SCHEME_API bool is_continuation(Value val);
SCHEME_API bool is_vector(Value val);
SCHEME_API bool is_bytevector(Value val);
SCHEME_API bool is_builtin(Value val);
SCHEME_API bool is_char(Value val);
SCHEME_API bool is_eof(Value val);
SCHEME_API bool is_complex(Value val);
SCHEME_API bool is_exact_complex(Value val);
SCHEME_API bool is_rational(Value val);
SCHEME_API bool is_exact_num(Value val);    // fixnum or rational
SCHEME_API bool is_multi_values(Value val);
SCHEME_API bool is_record(Value val);
SCHEME_API bool is_error_object(Value val);
SCHEME_API bool is_port(Value val);
SCHEME_API bool is_input_port(Value val);
SCHEME_API bool is_output_port(Value val);
SCHEME_API bool is_unspecified(Value val);
SCHEME_API bool is_procedure(Value val);    // builtin, closure, case-closure, continuation, parameter
SCHEME_API bool is_promise(Value val);
SCHEME_API bool is_parameter(Value val);
SCHEME_API bool is_environment(Value val);
SCHEME_API bool is_truthy(Value val);

// --- Constructors ---
SCHEME_API Value make_nil();
SCHEME_API Value make_bool(bool b);
SCHEME_API Value make_fixnum(int64_t n);
SCHEME_API Value make_flonum(double x);
SCHEME_API Value make_char(char32_t cp);
SCHEME_API Value make_eof();
SCHEME_API Value make_symbol_id(uint32_t sid);
SCHEME_API Value make_symbol(std::string_view name);   // convenience: intern + make_symbol_id
SCHEME_API Value make_cons(ConsCell* p);
SCHEME_API Value make_string(SchemeString* p);
SCHEME_API Value make_closure(SchemeClosure* p);
SCHEME_API Value make_case_closure(SchemeCaseClosure* p);
SCHEME_API Value make_macro(SchemeMacro* p);
SCHEME_API Value make_syntax_transformer(SchemeSyntaxTransformer* p);
SCHEME_API Value make_continuation(SchemeContinuation* p);
SCHEME_API Value make_vector(SchemeVector* p);
SCHEME_API Value make_bytevector(SchemeBytevector* p);
SCHEME_API Value make_builtin(Builtin* p);
SCHEME_API Value make_unspecified();
SCHEME_API Value make_promise_val(SchemePromise* p);
SCHEME_API Value make_parameter_val(SchemeParameter* p);
SCHEME_API Value make_port(SchemePort* p);
SCHEME_API Value make_environment_val(Environment* p);
SCHEME_API Value make_complex_val(double real, double imag);
SCHEME_API Value make_rational(SchemeRational* p);
SCHEME_API Value make_multi_values_val(SchemeMultiValues* p);
SCHEME_API Value make_record_val(SchemeRecord* p);
SCHEME_API Value make_exact_complex_val(SchemeExactComplex* p);
SCHEME_API Value make_error_object_val(SchemeErrorObject* p);

// --- Accessors (throw SchemeTypeError on wrong type) ---
SCHEME_API bool                  as_bool(Value val);
SCHEME_API int64_t               as_fixnum(Value val);
SCHEME_API double                as_flonum(Value val);
SCHEME_API char32_t              as_char(Value val);
SCHEME_API uint32_t              as_symbol_id(Value val);
SCHEME_API ConsCell*             as_cons(Value val);
SCHEME_API SchemeString*         as_string(Value val);
SCHEME_API SchemeClosure*        as_closure(Value val);
SCHEME_API SchemeCaseClosure*    as_case_closure(Value val);
SCHEME_API SchemeMacro*          as_macro(Value val);
SCHEME_API SchemeSyntaxTransformer* as_syntax_transformer(Value val);
SCHEME_API SchemeContinuation*   as_continuation(Value val);
SCHEME_API SchemeVector*         as_vector(Value val);
SCHEME_API SchemeBytevector*     as_bytevector(Value val);
SCHEME_API Builtin*              as_builtin(Value val);
SCHEME_API SchemePromise*        as_promise(Value val);
SCHEME_API SchemeParameter*      as_parameter(Value val);
SCHEME_API SchemePort*           as_port(Value val);
SCHEME_API Environment*          as_environment_val(Value val);
SCHEME_API SchemeComplex*        as_complex(Value val);
SCHEME_API SchemeRational*       as_rational(Value val);
SCHEME_API SchemeMultiValues*    as_multi_values(Value val);
SCHEME_API SchemeRecord*         as_record(Value val);
SCHEME_API SchemeExactComplex*   as_exact_complex(Value val);
SCHEME_API SchemeErrorObject*    as_error_object(Value val);
SCHEME_API double                to_double(Value val);    // fixnum or flonum -> double

// --- Pair operations ---
SCHEME_API Value car(Value val);
SCHEME_API Value cdr(Value val);
SCHEME_API void  set_car(Value cons_val, Value new_car);
SCHEME_API void  set_cdr(Value cons_val, Value new_cdr);

// --- Equality ---
SCHEME_API bool values_eq(Value lhs, Value rhs);
SCHEME_API bool values_eqv(Value lhs, Value rhs);
SCHEME_API bool values_equal(Value lhs, Value rhs);

// --- Display ---
SCHEME_API std::string value_to_string(Value val, bool display_mode = false);
SCHEME_API void        write_to_stream(Value val, std::ostream& out);
SCHEME_API void        write_shared_to_stream(Value val, std::ostream& out);
SCHEME_API std::string write_shared_to_string(Value val);

// --- GC support for moving collectors ---
SCHEME_API GcHeader* gc_value_header(Value val);
SCHEME_API void      gc_forward_value(Value& val);

// --- Arithmetic helpers ---
SCHEME_API Value num_add(Value lhs, Value rhs);
SCHEME_API Value num_sub(Value lhs, Value rhs);
SCHEME_API Value num_mul(Value lhs, Value rhs);
SCHEME_API Value num_div(Value lhs, Value rhs);
SCHEME_API bool  num_lt(Value lhs, Value rhs);
SCHEME_API bool  num_le(Value lhs, Value rhs);
SCHEME_API bool  num_gt(Value lhs, Value rhs);
SCHEME_API bool  num_ge(Value lhs, Value rhs);
SCHEME_API bool  num_eq(Value lhs, Value rhs);
