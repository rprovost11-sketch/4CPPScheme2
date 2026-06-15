#pragma once
// AST.h -- cons-cell AST and source-position infrastructure.
// Direct port of pyscheme/AST.py.
//
// The undercarriage (GcHeader, heap structs, Value variant) is adapted from
// CPPScheme's value.h/gc.h to expose exactly the API that AST.py defines.
// No other source file may access the Value::repr field or heap struct internals
// directly -- use the public make_X / is_X / as_X functions declared below.

#include "scheme_export.h"
#include "mini-gmp/mini-gmp.h"
#include <cstdint>
#include <variant>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <iosfwd>

// ── Value tag constants ───────────────────────────────────────────────────────
// Direct port of AST.py tag constants.  The GC uses GcType (below) to identify
// heap objects; these integer constants are used in src_of() switch logic and
// document the protocol that both pyscheme and cppscheme2 share.

constexpr int VOID = 0;
constexpr int BOOLEAN = 1;
constexpr int COMPLEX = 2;
constexpr int REAL = 3;
constexpr int RATIONAL = 4;
constexpr int INTEGER = 5;
constexpr int CHARACTER = 6;
constexpr int STRING = 7;
constexpr int CLOSURE = 8;
constexpr int PAIR = 9;
constexpr int NIL = 10;
constexpr int PRIMITIVE = 11;
constexpr int CASE_CLOSURE = 12;
constexpr int PROMISE = 13;
constexpr int MULTI_VALUES = 14;
constexpr int RECORD = 15;
constexpr int PARAMETER = 16;
constexpr int CONTINUATION = 17;
constexpr int SYNTAX_TRANSFORMER = 18;
constexpr int ENVIRONMENT = 19;
constexpr int RECORD_ACCESSOR = 20;
constexpr int RECORD_MUTATOR = 21;
constexpr int VECTOR = 22;
constexpr int BYTEVECTOR = 23;
constexpr int PORT = 24;
constexpr int EOF_TAG = 25; // 'EOF' conflicts with stdio.h macro
constexpr int EXACT_COMPLEX = 26;
constexpr int BIGNUM = 27;
constexpr int SYMBOL = 100;

// ── REPL filename sentinel ────────────────────────────────────────────────────
// Port of AST.py: REPL_FILENAME = '<repl>'
inline const char* const REPL_FILENAME = "<repl>";

// ── GC object type tags ───────────────────────────────────────────────────────
// One tag per GC-managed heap type.  Used by the GC to know how to trace
// each object's children.  Does not include immediate (non-heap) Value arms.

enum class GcType : uint8_t
   {
   Cons,
   String,
   Closure,
   CaseClosure,
   Promise,
   MultiValues,
   Record,
   RecordType,
   Parameter,
   ErrorObject,
   Continuation,
   SyntaxTransformer,
   Vector,
   Bytevector,
   Port,
   Complex,
   ExactComplex,
   Rational,
   RecordAccessor,
   RecordMutator,
   Environment,
   EnvBox,  // thin indirection wrapper for first-class environment values
   Bignum,  // arbitrary-precision integer (mini-gmp mpz)
   Integer, // boxed exact integer (int64_t)
   Real,    // boxed inexact real (double)
   Char,    // boxed character (char32_t)
   NativeClosure, // GC-managed native thunk: stateless fn + traced Value captures
   AliasCell, // macro free-identifier indirection: target sid + def env + copy
   };

// ── GC intrusive header ───────────────────────────────────────────────────────
// Every GC-managed heap object must begin with this header.
// marked and gen are plain scalars: the collector runs entirely on the mutator
// thread (incremental marking on one loop, no background/concurrent marker), so
// no atomic synchronization is needed.

struct GcHeader
   {
   bool marked = false;
   uint8_t gen = 0; // 0=young, 1=old
   GcType type;
   GcHeader* next = nullptr;
   GcHeader* forward = nullptr; // forwarding pointer (generational minor GC)

   explicit GcHeader(GcType t) : type(t) {}

   GcHeader(const GcHeader&) = delete;
   GcHeader& operator=(const GcHeader&) = delete;
   };

// ── SourceInfo ────────────────────────────────────────────────────────────────
// Port of AST.py SourceInfo class.  POD; not GC-managed.
// Owned 1:1 by its ConsCell (freed with the cell).
// empty string = Python None for filename.

struct SourceInfo
   {
   int line;
   int col;
   std::string source_line;
   std::string filename; // empty = no filename (Python None)

   SourceInfo(int l, int c, std::string sl, std::string fn)
       : line(l), col(c), source_line(std::move(sl)), filename(std::move(fn)) {}
   };

// ── Forward declaration of Context (defined in Context.h) ────────────────────
struct Context;

// ── Forward declarations of all heap object types ────────────────────────────
struct ConsCell;
struct SchemeString;
struct SchemeClosure;
struct CaseClosure;
struct Promise;
struct MultiValues;
struct Record;
struct RecordType;
struct Parameter;
struct ErrorObject;
struct Continuation;
struct SyntaxTransformer;
struct SchemeVector;
struct SchemeBytevector;
struct Port;
struct SchemeComplex;
struct ExactComplex;
struct SchemeRational;
struct SchemeBignum;
struct SchemeInteger;
struct SchemeReal;
struct SchemeChar;
struct RecordAccessor;
struct RecordMutator;
struct Environment;
struct EnvBox;
struct Builtin;
struct NativeClosure;
struct AliasCell;
struct Value;

// ── Sentinels for immediate (non-heap) Value arms ─────────────────────────────
struct VoidTag
   {
   }; // VOID arm
struct EofTag
   {
   }; // EOF arm

// ── Atom types (Option A: each atom carries SourceInfo*) ─────────────────────
// Port of AST.py tagged-tuple atoms: (INTEGER, n, src), (SYMBOL, sid, src), etc.
// Every atom struct carries a src pointer so src_of() returns parse position.
struct NilAtom
   {
   SourceInfo* src = nullptr;
   };
struct BoolAtom
   {
   bool value = false;
   SourceInfo* src = nullptr;
   };
struct SymbolAtom
   {
   uint32_t id = 0;
   SourceInfo* src = nullptr;
   };

// ── THE Value type ────────────────────────────────────────────────────────────
// Variant-based implementation.  Direct port of AST.py's tagged-tuple design.
// Each arm corresponds to one value kind in AST.py.
// No code outside AST.cpp may access Value::repr -- use is_X / as_X.

struct Value
   {
   using Repr = std::variant<
       NilAtom,            // NIL            (tag=10) -- default
       VoidTag,            // VOID           (tag=0)
       BoolAtom,           // BOOLEAN        (tag=1)
       SchemeInteger*,     // INTEGER        (tag=5)
       SchemeReal*,        // REAL           (tag=3)
       SchemeChar*,        // CHARACTER      (tag=6)
       EofTag,             // EOF            (tag=25)
       SymbolAtom,         // SYMBOL         (tag=100) -- intern id
       ConsCell*,          // PAIR           (tag=9)
       SchemeString*,      // STRING         (tag=7)
       SchemeClosure*,     // CLOSURE        (tag=8)
       CaseClosure*,       // CASE_CLOSURE   (tag=12)
       Promise*,           // PROMISE        (tag=13)
       MultiValues*,       // MULTI_VALUES   (tag=14)
       Record*,            // RECORD         (tag=15)
       RecordType*,        // record type descriptor (standalone; used by is_record_type)
       Parameter*,         // PARAMETER      (tag=16)
       ErrorObject*,       // error object   (raised value from `error`)
       Continuation*,      // CONTINUATION   (tag=17)
       SyntaxTransformer*, // SYNTAX_TRANSFORMER (tag=18)
       EnvBox*,            // ENVIRONMENT    (tag=19) -- first-class wrapped env
       RecordAccessor*,    // RECORD_ACCESSOR (tag=20)
       RecordMutator*,     // RECORD_MUTATOR  (tag=21)
       SchemeVector*,      // VECTOR         (tag=22)
       SchemeBytevector*,  // BYTEVECTOR     (tag=23)
       Port*,              // PORT           (tag=24)
       SchemeComplex*,     // COMPLEX        (tag=2)  -- inexact (double components)
       ExactComplex*,      // EXACT_COMPLEX  (tag=26) -- exact (Value components)
       SchemeRational*,    // RATIONAL       (tag=4)
       SchemeBignum*,      // BIGNUM         (tag=27)
       Builtin*,           // PRIMITIVE      (tag=11)
       NativeClosure*,     // native thunk with GC-traced captures (internal)
       AliasCell*          // macro free-identifier indirection (internal)
       >;
   Repr repr;

   Value() : repr(NilAtom{}) {}
   explicit Value(Repr inner) : repr(std::move(inner)) {}
   };

// ── Builtin procedure ────────────────────────────────────────────────────────
// Port of AST.py PRIMITIVE arm.  Statically allocated; not GC-managed.
// Signature: fn(ctx, env, args, app_node) matching Python's primitive convention.
// app_node may be nullptr (no call-site position available).

using BuiltinFn = std::function<Value(Context*, Environment*, std::vector<Value>&, const Value*)>;

// Primitive kinds (#2).  PRIM_ORDINARY (0) is the common case; nonzero
// kinds mark the special primitives the evaluator intercepts at the
// FRAME_CALL application point.  Stored on the Builtin so the evaluator
// dispatches on one integer instead of ~15 name comparisons per call.
// Port of AST.py PRIM_* / _PRIMITIVE_KIND_BY_NAME.
constexpr int PRIM_ORDINARY               = 0;
constexpr int PRIM_CALL_CC                = 1;
constexpr int PRIM_APPLY                  = 2;
constexpr int PRIM_CALL_WITH_VALUES       = 3;
constexpr int PRIM_FORCE                  = 4;
constexpr int PRIM_MAKE_PARAMETER         = 5;
constexpr int PRIM_WITH_EXCEPTION_HANDLER = 6;
constexpr int PRIM_GUARD_EVAL             = 7;
constexpr int PRIM_RAISE                  = 8;
constexpr int PRIM_RAISE_CONTINUABLE      = 9;
constexpr int PRIM_EVAL                   = 10;
constexpr int PRIM_ERROR                  = 11;
constexpr int PRIM_WITH_PARAMETERS        = 12;
constexpr int PRIM_DYNAMIC_WIND           = 13;
constexpr int PRIM_CONTINUATION_DEPTH     = 14;
constexpr int PRIM_MAP                     = 15;
constexpr int PRIM_FOR_EACH                = 16;
constexpr int PRIM_FILTER                  = 17;
constexpr int PRIM_VECTOR_MAP             = 18;
constexpr int PRIM_VECTOR_FOR_EACH        = 19;
constexpr int PRIM_STRING_MAP             = 20;
constexpr int PRIM_STRING_FOR_EACH        = 21;
constexpr int PRIM_MEMBER                 = 22;
constexpr int PRIM_ASSOC                  = 23;
constexpr int PRIM_PORT_RUNNER            = 24;
constexpr int PRIM_LOAD                   = 25;

struct Builtin
   {
   std::string name;
   BuiltinFn fn;
   int kind = PRIM_ORDINARY;

   Builtin(std::string n, BuiltinFn f)
       : name(std::move(n)), fn(std::move(f)) {}
   };

// ── Native closure ────────────────────────────────────────────────────────────
// A GC-managed native thunk used for internal wind / cleanup work (parameterize
// install/restore, port-runner cleanup, dynamic-wind native afters).  Unlike a
// Builtin (a stable, never-collected global whose std::function captures are
// OPAQUE to the GC), a NativeClosure is a GC object whose captured Values live in
// an explicit `captures` vector the collector traces and forwards -- so capturing
// young Scheme values is safe, and the closure is reclaimed when unreachable.
// fn is STATELESS (a function pointer): the state is passed in via captures.
using NativeFn = Value (*)(Context*, Environment*, std::vector<Value>& args,
                           const std::vector<Value>& captures, const Value* app);

struct NativeClosure
   {
   GcHeader header{GcType::NativeClosure};
   std::string name;
   NativeFn fn = nullptr;
   std::vector<Value> captures;

   NativeClosure() = default;
   NativeClosure(const NativeClosure&) = delete;
   NativeClosure& operator=(const NativeClosure&) = delete;
   };

// ── Macro free-identifier alias cell ───────────────────────────────────────────
// Port of Environment.py _AliasCell.  A syntax-rules template's free reference to
// a binding that existed at macro definition time is emitted as a fresh gensym
// (so a same-named use-site binding cannot capture it -- hygiene); that gensym is
// bound, in the global env's _bindings, to one of these instead of to a copy of
// the referent's value.  Resolving prefers the LIVE def-site binding (`target` in
// `def_env`), so set! through the macro writes through and later mutations are
// seen; if def_env no longer resolves target at eval time (a transient body-scan
// scope, as for library-internal helpers), it falls back to `copy`, the def-time
// snapshot.  Stored as a Value in _bindings so it rides through the library
// export/import machinery unchanged (lookup resolves it to a plain value).
// Never escapes to user code: the lookup/set paths resolve it in place.
struct AliasCell
   {
   GcHeader header{GcType::AliasCell};
   uint32_t target = 0;        // the referent's symbol id
   Environment* def_env = nullptr; // macro definition environment
   Value copy;                 // def-time value snapshot (fallback)

   AliasCell() = default;
   AliasCell(const AliasCell&) = delete;
   AliasCell& operator=(const AliasCell&) = delete;
   };

// ── Dynamic-wind frame ────────────────────────────────────────────────────────
// One entry on the dynamic-wind stack.  Both fields are Scheme callables.
// Port of the (before_thunk, after_thunk) pairs in Evaluator.py's wind stack.

struct WindFrame
   {
   Value before;
   Value after;
   };

// ── Shadow-stack entry ────────────────────────────────────────────────────────
// Port of Evaluator.py shadow stack [label, src, count] mutable lists.
// Kept in Context::shadow_stack (live) and encoded as cons cells in
// Continuation::shadow_snapshot (GC-safe snapshot).

struct ShadowEntry
   {
   std::string label;
   SourceInfo* src; // borrowed from the call-site cons cell; not owned
   int count;
   };

// ── Heap object definitions ───────────────────────────────────────────────────
// Each struct begins with GcHeader so the GC can walk the allocation list.
// Struct names match AST.py class names where they exist.

// Port of AST.py ConsCell class.
struct ConsCell
   {
   GcHeader header{GcType::Cons};
   Value car{};
   Value cdr{};
   SourceInfo* src = nullptr; // optional; owned by this cell
   bool immutable = false;

   ConsCell() = default;
   ~ConsCell()
      {
      delete src;
      }
   ConsCell(const ConsCell&) = delete;
   ConsCell& operator=(const ConsCell&) = delete;
   };

// Port of AST.py SchemeString class.
// C port comment: struct SchemeString { GcHeader h; char* s; size_t len; bool immutable; };
struct SchemeString
   {
   GcHeader header{GcType::String};
   std::string data;
   SourceInfo* src = nullptr; // optional; owned by this string
   bool immutable = false;

   explicit SchemeString(std::string content, SourceInfo* s = nullptr)
       : data(std::move(content)), src(s) {}
   ~SchemeString()
      {
      delete src;
      }
   SchemeString(const SchemeString&) = delete;
   SchemeString& operator=(const SchemeString&) = delete;
   };

// Port of AST.py make_closure / CLOSURE arm.
// params:       positional parameter symbol ids
// body:         cons chain of body expressions
// env:          captured lexical environment
// rest_name_id: symbol id for variadic rest param, or UINT32_MAX if none
// docstring:    for display only
struct SchemeClosure
   {
   GcHeader header{GcType::Closure};
   std::vector<uint32_t> params;
   uint32_t rest_name_id; // UINT32_MAX = no rest param
   Value body;
   Environment* env;
   std::string docstring;

   SchemeClosure() : rest_name_id(UINT32_MAX), env(nullptr) {}
   SchemeClosure(const SchemeClosure&) = delete;
   SchemeClosure& operator=(const SchemeClosure&) = delete;
   };

// Port of AST.py make_case_closure / CASE_CLOSURE arm.
struct CaseClosure
   {
   GcHeader header{GcType::CaseClosure};
   struct Clause
      {
      std::vector<uint32_t> params;
      uint32_t rest_name_id; // UINT32_MAX = no rest param
      Value body;
      };
   std::vector<Clause> clauses;
   Environment* env;
   std::string docstring;

   CaseClosure() : env(nullptr) {}
   CaseClosure(const CaseClosure&) = delete;
   CaseClosure& operator=(const CaseClosure&) = delete;
   };

// Port of AST.py Promise class.
struct Promise
   {
   GcHeader header{GcType::Promise};
   bool is_done;
   bool iterative; // delay-force: force tail-chases into a promise result;
                   // plain delay: force resolves to the value as-is
   Value payload;  // thunk (CLOSURE) if !is_done; memoized result if is_done

   Promise(bool done, Value p, bool iter = false)
       : is_done(done), iterative(iter), payload(std::move(p)) {}
   Promise(const Promise&) = delete;
   Promise& operator=(const Promise&) = delete;
   };

// Port of AST.py make_multi_values.
struct MultiValues
   {
   GcHeader header{GcType::MultiValues};
   std::vector<Value> values;
   SourceInfo* src = nullptr; // optional; owned

   MultiValues() = default;
   ~MultiValues()
      {
      delete src;
      }
   MultiValues(const MultiValues&) = delete;
   MultiValues& operator=(const MultiValues&) = delete;
   };

// Port of AST.py RecordType class.  Descriptor shared by identity among all
// records of the same define-record-type.
struct RecordType
   {
   GcHeader header{GcType::RecordType};
   std::string name;
   std::vector<uint32_t> field_name_ids; // symbol ids of field names

   RecordType() = default;
   RecordType(const RecordType&) = delete;
   RecordType& operator=(const RecordType&) = delete;
   };

// Port of AST.py make_record / RECORD arm.
struct Record
   {
   GcHeader header{GcType::Record};
   RecordType* record_type;
   std::vector<Value> field_values;

   Record() : record_type(nullptr) {}
   Record(const Record&) = delete;
   Record& operator=(const Record&) = delete;
   };

// Port of AST.py Parameter class.
struct Parameter
   {
   GcHeader header{GcType::Parameter};
   Value value;     // current dynamic value
   Value converter; // NIL or a 1-arg procedure

   Parameter() = default;
   Parameter(const Parameter&) = delete;
   Parameter& operator=(const Parameter&) = delete;
   };

// Port of AST.py ErrorObject class.
// kind: 0=generic, 1=file-error, 2=read-error
struct ErrorObject
   {
   GcHeader header{GcType::ErrorObject};
   std::string message;
   std::vector<Value> irritants;
   int kind;

   ErrorObject(std::string msg, std::vector<Value> irr, int k = 0)
       : message(std::move(msg)), irritants(std::move(irr)), kind(k) {}
   ErrorObject(const ErrorObject&) = delete;
   ErrorObject& operator=(const ErrorObject&) = delete;
   };

// Port of AST.py Continuation class.
// frames_ptr: opaque owning pointer to KStack snapshot; destructor in Evaluator.cpp.
struct Continuation
   {
   GcHeader header{GcType::Continuation};
   void* frames_ptr = nullptr;
   std::vector<WindFrame> wind_snapshot;
   std::vector<Value> handler_snapshot;
   std::vector<Value> shadow_snapshot;

   Continuation() = default;
   ~Continuation();
   Continuation(const Continuation&) = delete;
   Continuation& operator=(const Continuation&) = delete;
   };

// Port of AST.py SyntaxTransformer class.
// Uses uint32_t symbol ids (not Symbol*) to match AST.py's intern-pool design.
// free_id_map and intro_names carry cppscheme2's hygiene model.
struct SyntaxTransformer
   {
   GcHeader header{GcType::SyntaxTransformer};
   std::string name;
   std::vector<uint32_t> literals;
   uint32_t ellipsis_id;
   struct Rule
      {
      Value pattern;
      Value tmpl;
      };
   std::vector<Rule> rules;
   std::unordered_map<uint32_t, uint32_t> free_id_map;
   std::unordered_set<uint32_t> intro_names;
   std::unordered_set<uint32_t> hygienic_intro_names;

   SyntaxTransformer() : ellipsis_id(0) {}
   SyntaxTransformer(const SyntaxTransformer&) = delete;
   SyntaxTransformer& operator=(const SyntaxTransformer&) = delete;
   };

// Port of AST.py Port class.
struct Port
   {
   GcHeader header{GcType::Port};
   std::string buf_text;            // text input: unconsumed chars; text output: chunks
   std::vector<uint8_t> buf_binary; // binary port buffer
   size_t pos = 0;
   bool is_input;
   bool is_text;
   std::FILE* file_h = nullptr;
   std::string name;
   bool is_open = true;
   // True for the <stdin> port whose buffer has not been filled yet: the first
   // read slurps all of stdin into buf_text (the up-front model open-input-file
   // uses), then clears the flag.
   bool from_stdin = false;

   Port(bool input, bool text, std::string n)
       : is_input(input), is_text(text), name(std::move(n)) {}
   Port(const Port&) = delete;
   Port& operator=(const Port&) = delete;
   };

// Inexact complex number (double components).
// Invariant: imag != 0.0 (use REAL if imag==0).
struct SchemeComplex
   {
   GcHeader header{GcType::Complex};
   double real;
   double imag;

   SchemeComplex(double r, double i) : real(r), imag(i) {}
   SchemeComplex(const SchemeComplex&) = delete;
   SchemeComplex& operator=(const SchemeComplex&) = delete;
   };

// Exact complex number (INTEGER or RATIONAL Value components).
// Port of AST.py EXACT_COMPLEX arm.
struct ExactComplex
   {
   GcHeader header{GcType::ExactComplex};
   Value re; // INTEGER or RATIONAL Value
   Value im; // INTEGER or RATIONAL Value (non-zero)

   ExactComplex(Value r, Value i) : re(std::move(r)), im(std::move(i)) {}
   ExactComplex(const ExactComplex&) = delete;
   ExactComplex& operator=(const ExactComplex&) = delete;
   };

// Exact rational in lowest terms.  Invariants: den >= 2, den > 0, gcd(|num|,den)==1.
struct SchemeRational
   {
   GcHeader header{GcType::Rational};
   int64_t num;
   int64_t den;

   SchemeRational(int64_t n, int64_t d) : num(n), den(d) {}
   SchemeRational(const SchemeRational&) = delete;
   SchemeRational& operator=(const SchemeRational&) = delete;
   };

// Port of AST.py RECORD_ACCESSOR arm.
struct RecordAccessor
   {
   GcHeader header{GcType::RecordAccessor};
   RecordType* record_type;
   int index;
   std::string name;

   RecordAccessor(RecordType* rt, int i, std::string n)
       : record_type(rt), index(i), name(std::move(n)) {}
   RecordAccessor(const RecordAccessor&) = delete;
   RecordAccessor& operator=(const RecordAccessor&) = delete;
   };

// Port of AST.py RECORD_MUTATOR arm.
struct RecordMutator
   {
   GcHeader header{GcType::RecordMutator};
   RecordType* record_type;
   int index;
   std::string name;

   RecordMutator(RecordType* rt, int i, std::string n)
       : record_type(rt), index(i), name(std::move(n)) {}
   RecordMutator(const RecordMutator&) = delete;
   RecordMutator& operator=(const RecordMutator&) = delete;
   };

// Mutable vector of Values.
// C port comment: struct SchemeVector { GcHeader h; Value* data; size_t len; bool immutable; };
struct SchemeVector
   {
   GcHeader header{GcType::Vector};
   std::vector<Value> elements;
   bool immutable = false;

   explicit SchemeVector(size_t n, Value fill = Value{}) : elements(n, fill) {}
   SchemeVector(const SchemeVector&) = delete;
   SchemeVector& operator=(const SchemeVector&) = delete;
   };

// Mutable vector of uint8 values.
// C port comment: struct SchemeBytevector { GcHeader h; uint8_t* data; size_t len; bool immutable; };
struct SchemeBytevector
   {
   GcHeader header{GcType::Bytevector};
   std::vector<uint8_t> data;
   bool immutable = false;

   SchemeBytevector() = default;
   explicit SchemeBytevector(size_t n, uint8_t fill = 0) : data(n, fill) {}
   SchemeBytevector(const SchemeBytevector&) = delete;
   SchemeBytevector& operator=(const SchemeBytevector&) = delete;
   };

// Thin GC-allocated wrapper around Environment* for first-class env values.
// Each call to (interaction-environment) or make_environment allocates a fresh
// EnvBox so that eq? compares wrapper identity (not the underlying env pointer).
struct EnvBox
   {
   GcHeader header{GcType::EnvBox};
   Environment* env;

   explicit EnvBox(Environment* e) : env(e) {}
   EnvBox(const EnvBox&) = delete;
   EnvBox& operator=(const EnvBox&) = delete;
   };

// Arbitrary-precision exact integer.
// Wraps a mini-gmp mpz value.  The GC calls mpz_clear before deleting.
struct SchemeBignum
   {
   GcHeader header{GcType::Bignum};
   __mpz_struct value; // initialized via mpz_init in gc_alloc_bignum

   SchemeBignum(const SchemeBignum&) = delete;
   SchemeBignum& operator=(const SchemeBignum&) = delete;
   };

// Boxed exact integer — pool-allocated, GC-managed.
struct SchemeInteger
   {
   GcHeader header{GcType::Integer};
   int64_t value = 0;
   SourceInfo* src = nullptr;

   SchemeInteger(const SchemeInteger&) = delete;
   SchemeInteger& operator=(const SchemeInteger&) = delete;
   };

// Boxed inexact real — pool-allocated, GC-managed.
struct SchemeReal
   {
   GcHeader header{GcType::Real};
   double value = 0.0;
   SourceInfo* src = nullptr;

   SchemeReal(const SchemeReal&) = delete;
   SchemeReal& operator=(const SchemeReal&) = delete;
   };

// Boxed character — pool-allocated, GC-managed.
struct SchemeChar
   {
   GcHeader header{GcType::Char};
   char32_t value = 0;
   SourceInfo* src = nullptr;

   SchemeChar(const SchemeChar&) = delete;
   SchemeChar& operator=(const SchemeChar&) = delete;
   };

// ── Symbol intern pool ────────────────────────────────────────────────────────
// Port of AST.py _SYMBOL_POOL / _SYMBOL_NAMES.
// Maps symbol name strings to stable uint32_t ids.

CPPSCHEME2_API uint32_t intern_symbol(const std::string& name);
CPPSCHEME2_API std::string symbol_name(uint32_t sid);

// Hygiene gensym name format: GENSYM_PREFIX + base + '.' + counter, where the
// prefix is a non-printable marker byte so display / error paths can strip it.
// The ENCODER (which owns the counter) is syntax_rules' hygiene_gensym; this
// prefix and gensym_display_name (its inverse) are the single source shared by
// the analyzer / environment / pretty-printer display paths.
extern const std::string GENSYM_PREFIX;
CPPSCHEME2_API std::string gensym_display_name(const std::string& name);

// Hygiene marks (A1/A3): a per-expansion stamp suffixed onto template-introduced
// identifiers (name + MARK_PREFIX + mark-id).  paint_mark appends one; strip_marks
// removes all (the base / resolution name).  See AST.cpp for the model.
extern const std::string MARK_PREFIX;
CPPSCHEME2_API std::string paint_mark(const std::string& name, uint64_t mark_id);
CPPSCHEME2_API std::string strip_marks(const std::string& name);

// ── Singletons ────────────────────────────────────────────────────────────────
// Port of AST.py NIL_VALUE, VOID_VALUE, EOF_VALUE.

CPPSCHEME2_API extern const Value NIL_VALUE;
CPPSCHEME2_API extern const Value VOID_VALUE;
CPPSCHEME2_API extern const Value EOF_VALUE;

// ── Allocators ────────────────────────────────────────────────────────────────
// Port of AST.py make_X functions.

CPPSCHEME2_API Value alloc_cons(Value car_val, Value cdr_val, SourceInfo* src = nullptr);
CPPSCHEME2_API Value make_nil(SourceInfo* src = nullptr);
CPPSCHEME2_API Value make_void();
CPPSCHEME2_API Value make_boolean(bool b, SourceInfo* src = nullptr);
CPPSCHEME2_API Value make_integer(int64_t n, SourceInfo* src = nullptr);
CPPSCHEME2_API Value make_real(double x, SourceInfo* src = nullptr);
CPPSCHEME2_API Value make_rational(int64_t num, int64_t den, SourceInfo* src = nullptr);
CPPSCHEME2_API Value make_complex(double re, double im, SourceInfo* src = nullptr);
CPPSCHEME2_API Value make_exact_complex(Value re, Value im, SourceInfo* src = nullptr);
CPPSCHEME2_API Value make_character(char32_t c, SourceInfo* src = nullptr);
CPPSCHEME2_API Value make_string(const std::string& s, SourceInfo* src = nullptr);

// UTF-8 helpers (shared by the string primitives and the evaluator's HOF
// frames).  utf8_next advances pos past one code point and returns it;
// utf8_encode appends one code point's UTF-8 bytes to out.
CPPSCHEME2_API char32_t utf8_next(const std::string& s, size_t& pos);
CPPSCHEME2_API void utf8_encode(std::string& out, char32_t c);
CPPSCHEME2_API Value make_symbol(const std::string& name, SourceInfo* src = nullptr);
CPPSCHEME2_API Value make_symbol_id(uint32_t sid, SourceInfo* src = nullptr);
CPPSCHEME2_API Value make_closure(std::vector<uint32_t> params, Value body,
                                  Environment* env, uint32_t rest_name_id,
                                  std::string docstring);
CPPSCHEME2_API Value make_primitive(const std::string& name, BuiltinFn fn);
// Build a GC-managed native closure (stateless fn + traced Value captures).
CPPSCHEME2_API Value make_native_closure(const std::string& name, NativeFn fn,
                                         std::vector<Value> captures);
// Build a macro free-identifier alias cell (see struct AliasCell).
CPPSCHEME2_API Value make_alias_cell(uint32_t target, Environment* def_env,
                                     Value copy);
CPPSCHEME2_API bool is_alias_cell(const Value& val);
CPPSCHEME2_API AliasCell* as_alias_cell(const Value& val);
CPPSCHEME2_API Value make_case_closure(std::vector<CaseClosure::Clause> clauses,
                                       Environment* env, std::string docstring);
CPPSCHEME2_API Value make_promise_lazy(Value thunk, bool iterative = false);
CPPSCHEME2_API Value make_promise_done(Value val);
CPPSCHEME2_API Value make_multi_values(std::vector<Value> vals, SourceInfo* src = nullptr);
CPPSCHEME2_API Value make_record_type(const std::string& name,
                                      std::vector<uint32_t> field_name_ids);
CPPSCHEME2_API Value make_record(RecordType* rt, std::vector<Value> field_values);
CPPSCHEME2_API Value make_parameter(Value val, Value converter);
CPPSCHEME2_API Value make_error_object(const std::string& message,
                                       std::vector<Value> irritants);
CPPSCHEME2_API Value make_file_error_object(const std::string& message,
                                            std::vector<Value> irritants);
CPPSCHEME2_API Value make_read_error_object(const std::string& message,
                                            std::vector<Value> irritants);
CPPSCHEME2_API Value make_continuation(void* frames_ptr,
                                       std::vector<WindFrame> wind_snapshot,
                                       std::vector<Value> handler_snapshot,
                                       std::vector<Value> shadow_snapshot);
CPPSCHEME2_API Value make_syntax_transformer(
    const std::string& name,
    std::vector<uint32_t> literals,
    uint32_t ellipsis_id,
    std::vector<SyntaxTransformer::Rule> rules,
    std::unordered_map<uint32_t, uint32_t> free_id_map,
    std::unordered_set<uint32_t> intro_names,
    std::unordered_set<uint32_t> hygienic_intro_names = {});
CPPSCHEME2_API Value make_environment(Environment* env);
CPPSCHEME2_API Value make_record_accessor(RecordType* rt, int index,
                                          const std::string& name);
CPPSCHEME2_API Value make_record_mutator(RecordType* rt, int index,
                                         const std::string& name);
CPPSCHEME2_API Value make_vector(std::vector<Value> items);
CPPSCHEME2_API Value make_bytevector(std::vector<uint8_t> items);
CPPSCHEME2_API Value make_port(bool is_input, bool is_text, const std::string& name);
CPPSCHEME2_API Value make_eof();
CPPSCHEME2_API Value make_bignum_si(int64_t n, SourceInfo* src = nullptr);
CPPSCHEME2_API Value make_bignum_str(const char* s, int base, SourceInfo* src = nullptr);
CPPSCHEME2_API Value make_bignum_copy(const __mpz_struct* z, SourceInfo* src = nullptr);
CPPSCHEME2_API void init_value_pools();
void pool_return_integer(SchemeInteger* p);
void pool_return_real(SchemeReal* p);
void pool_return_char(SchemeChar* p);

// ── Predicates ────────────────────────────────────────────────────────────────
// Port of AST.py is_X functions.

CPPSCHEME2_API bool is_cons(const Value& val);
CPPSCHEME2_API bool is_nil(const Value& val);
CPPSCHEME2_API bool is_void(const Value& val);
CPPSCHEME2_API bool is_boolean(const Value& val);
CPPSCHEME2_API bool is_integer(const Value& val);
CPPSCHEME2_API bool is_real(const Value& val);
CPPSCHEME2_API bool is_rational(const Value& val);
CPPSCHEME2_API bool is_complex(const Value& val);
CPPSCHEME2_API bool is_exact_complex(const Value& val);
CPPSCHEME2_API bool is_character(const Value& val);
CPPSCHEME2_API bool is_string(const Value& val);
CPPSCHEME2_API bool is_symbol(const Value& val);
CPPSCHEME2_API bool is_closure(const Value& val);
CPPSCHEME2_API bool is_primitive(const Value& val);
CPPSCHEME2_API bool is_case_closure(const Value& val);
CPPSCHEME2_API bool is_promise(const Value& val);
CPPSCHEME2_API bool is_multi_values(const Value& val);
CPPSCHEME2_API bool is_record_type(const Value& val);
CPPSCHEME2_API bool is_record(const Value& val);
CPPSCHEME2_API bool is_parameter(const Value& val);
CPPSCHEME2_API bool is_error_object(const Value& val);
CPPSCHEME2_API bool is_file_error_object(const Value& val);
CPPSCHEME2_API bool is_read_error_object(const Value& val);
CPPSCHEME2_API bool is_continuation(const Value& val);
CPPSCHEME2_API bool is_syntax_transformer(const Value& val);
CPPSCHEME2_API bool is_environment(const Value& val);
CPPSCHEME2_API bool is_record_accessor(const Value& val);
CPPSCHEME2_API bool is_record_mutator(const Value& val);
CPPSCHEME2_API bool is_vector(const Value& val);
CPPSCHEME2_API bool is_bytevector(const Value& val);
CPPSCHEME2_API bool is_port(const Value& val);
CPPSCHEME2_API bool is_eof(const Value& val);
CPPSCHEME2_API bool is_bignum(const Value& val);

// Convenience predicates not in AST.py but needed by other modules.
CPPSCHEME2_API bool is_truthy(const Value& val);    // everything except #f is truthy
CPPSCHEME2_API bool is_number(const Value& val);    // any numeric type
CPPSCHEME2_API bool is_procedure(const Value& val); // closure, case-closure, primitive, continuation, parameter

// ── Accessors ─────────────────────────────────────────────────────────────────
// Port of AST.py as_X functions.

CPPSCHEME2_API bool as_boolean(const Value& val);
CPPSCHEME2_API int64_t as_integer(const Value& val);
CPPSCHEME2_API SourceInfo* integer_src(const Value& val);
CPPSCHEME2_API double as_real(const Value& val);
CPPSCHEME2_API int64_t as_rational_num(const Value& val);
CPPSCHEME2_API int64_t as_rational_den(const Value& val);
CPPSCHEME2_API double as_complex_real(const Value& val);
CPPSCHEME2_API double as_complex_imag(const Value& val);
CPPSCHEME2_API Value as_exact_complex_real(const Value& val);
CPPSCHEME2_API Value as_exact_complex_imag(const Value& val);
CPPSCHEME2_API char32_t as_character(const Value& val);
CPPSCHEME2_API SourceInfo* character_src(const Value& val);
CPPSCHEME2_API const __mpz_struct* as_bignum(const Value& val);
CPPSCHEME2_API std::string bignum_to_string(const Value& val, int base = 10);
CPPSCHEME2_API const std::string& as_string(const Value& val);
CPPSCHEME2_API std::string& as_string_mut(Value& val);
CPPSCHEME2_API std::string as_symbol(const Value& val); // returns name string
CPPSCHEME2_API uint32_t as_symbol_id(const Value& val); // returns intern id

CPPSCHEME2_API const std::vector<uint32_t>& as_closure_params(const Value& val);
CPPSCHEME2_API Value as_closure_body(const Value& val);
CPPSCHEME2_API Environment* as_closure_env(const Value& val);
CPPSCHEME2_API uint32_t as_closure_rest_name(const Value& val); // UINT32_MAX=none
CPPSCHEME2_API const std::string& as_closure_docstring(const Value& val);

CPPSCHEME2_API const std::string& as_primitive_name(const Value& val);
CPPSCHEME2_API const BuiltinFn& as_primitive_fn(const Value& val);
CPPSCHEME2_API int as_primitive_kind(const Value& val);
CPPSCHEME2_API bool is_native_closure(const Value& val);
CPPSCHEME2_API NativeFn as_native_closure_fn(const Value& val);
CPPSCHEME2_API std::vector<Value>& as_native_closure_captures(const Value& val);
CPPSCHEME2_API const std::string& as_native_closure_name(const Value& val);

CPPSCHEME2_API const std::vector<CaseClosure::Clause>& as_case_closure_clauses(const Value& val);
CPPSCHEME2_API Environment* as_case_closure_env(const Value& val);
CPPSCHEME2_API const std::string& as_case_closure_docstring(const Value& val);

CPPSCHEME2_API bool as_promise_is_done(const Value& val);
CPPSCHEME2_API bool as_promise_is_iterative(const Value& val);
CPPSCHEME2_API Value as_promise_payload(const Value& val);
CPPSCHEME2_API void promise_resolve(Value& promise_val, Value result);
CPPSCHEME2_API void promise_become(Value& dst, const Value& src_val);

CPPSCHEME2_API const std::vector<Value>& as_multi_values_list(const Value& val);

CPPSCHEME2_API RecordType* as_record_type_obj(const Value& val); // for standalone RecordType* arm
CPPSCHEME2_API const std::string& as_record_type_name(const Value& val);
CPPSCHEME2_API const std::vector<uint32_t>& as_record_type_field_names(const Value& val);

CPPSCHEME2_API RecordType* as_record_type(const Value& record_val); // field of RECORD
CPPSCHEME2_API std::vector<Value>& as_record_fields(Value& val);
CPPSCHEME2_API const std::vector<Value>& as_record_fields_const(const Value& val);

CPPSCHEME2_API Value as_parameter_value(const Value& val);
CPPSCHEME2_API Value as_parameter_converter(const Value& val);
CPPSCHEME2_API void set_parameter_value(Value& val, Value newval);

CPPSCHEME2_API const std::string& as_error_object_message(const Value& val);
CPPSCHEME2_API const std::vector<Value>& as_error_object_irritants(const Value& val);
CPPSCHEME2_API int as_error_object_kind(const Value& val);

CPPSCHEME2_API void* as_continuation_frames(const Value& val);
CPPSCHEME2_API const std::vector<WindFrame>& as_continuation_wind(const Value& val);
CPPSCHEME2_API const std::vector<Value>& as_continuation_handlers(const Value& val);
CPPSCHEME2_API const std::vector<Value>& as_continuation_shadow(const Value& val);

CPPSCHEME2_API const std::string& as_syntax_transformer_name(const Value& val);
CPPSCHEME2_API const std::vector<uint32_t>& as_syntax_transformer_literals(const Value& val);
CPPSCHEME2_API uint32_t as_syntax_transformer_ellipsis(const Value& val);
CPPSCHEME2_API const std::vector<SyntaxTransformer::Rule>& as_syntax_transformer_rules(const Value& val);
CPPSCHEME2_API const std::unordered_map<uint32_t, uint32_t>& as_syntax_transformer_free_id_map(const Value& val);
CPPSCHEME2_API const std::unordered_set<uint32_t>& as_syntax_transformer_intro_names(const Value& val);
CPPSCHEME2_API const std::unordered_set<uint32_t>& as_syntax_transformer_hygienic_intro_names(const Value& val);

CPPSCHEME2_API Environment* as_environment(const Value& val);

CPPSCHEME2_API RecordType* as_record_accessor_type(const Value& val);
CPPSCHEME2_API int as_record_accessor_index(const Value& val);
CPPSCHEME2_API const std::string& as_record_accessor_name(const Value& val);

CPPSCHEME2_API RecordType* as_record_mutator_type(const Value& val);
CPPSCHEME2_API int as_record_mutator_index(const Value& val);
CPPSCHEME2_API const std::string& as_record_mutator_name(const Value& val);

CPPSCHEME2_API std::vector<Value>& as_vector_items(Value& val);
CPPSCHEME2_API const std::vector<Value>& as_vector_items_const(const Value& val);

CPPSCHEME2_API std::vector<uint8_t>& as_bytevector_items(Value& val);
CPPSCHEME2_API const std::vector<uint8_t>& as_bytevector_items_const(const Value& val);

CPPSCHEME2_API Port* as_port(const Value& val);

// ── Immutability ──────────────────────────────────────────────────────────────
// Port of AST.py mark_literal_immutable / is_immutable.

CPPSCHEME2_API bool is_immutable(const Value& val);
CPPSCHEME2_API void mark_literal_immutable(const Value& val);

// ── Pair operations ───────────────────────────────────────────────────────────
// Port of AST.py car, cdr, set_car, set_cdr.

CPPSCHEME2_API Value car(const Value& val);
CPPSCHEME2_API Value cdr(const Value& val);
CPPSCHEME2_API void set_car(Value& cons_val, Value new_car);
CPPSCHEME2_API void set_cdr(Value& cons_val, Value new_cdr);

// ── Value equality ────────────────────────────────────────────────────────────
// Port of AST.py eqv_atom.
CPPSCHEME2_API bool eqv_atom(const Value& a, const Value& b);

// ── List construction helper ──────────────────────────────────────────────────
// Port of AST.py list_from_items.
CPPSCHEME2_API Value list_from_items(const std::vector<Value>& items,
                                     SourceInfo* src = nullptr);

// ── Source info extraction ────────────────────────────────────────────────────
// Port of AST.py src_of.
CPPSCHEME2_API SourceInfo* src_of(const Value& val);

// ── Diagnostics ───────────────────────────────────────────────────────────────
// Port of AST.py format_with_caret.
CPPSCHEME2_API std::string format_with_caret(const std::string& msg, SourceInfo* src);

// ── GC support ────────────────────────────────────────────────────────────────
// Returns the GcHeader* for val if it holds a GC-managed heap pointer, else nullptr.
CPPSCHEME2_API GcHeader* gc_value_header(const Value& val);

// If val's heap object has a forwarding pointer set, update val's pointer in-place.
CPPSCHEME2_API void gc_forward_value(Value& val);
