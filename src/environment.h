#pragma once
// Environment.h -- lexical environment and Scheme runtime-error hierarchy.
// Direct port of pyscheme/Environment.py.
#include "gc.h"
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// ── Non-error control escape ─────────────────────────────────────────────────
// ReplExitSignal unwinds an interactive evaluation back to the REPL top level
// when (exit) is called from the prompt rather than from a batch file.  Like
// _QuitSignal it is deliberately a plain struct -- NOT a
// std::exception and NOT a Scheme error -- so guard / with-exception-handler
// never intercept it and the generic catch(std::exception&) at the prompt does
// not swallow it (readEvalPrintLoop catches it by type).  Batch-mode (exit)
// still calls std::exit(); the mode is read from Context::interactive.
// Port of Environment.py ReplExit.  See primitives/meta.cpp:_prim_exit.
struct ReplExitSignal
   {
   int code = 0;
   };

// ── Scheme runtime error hierarchy ───────────────────────────────────────────
// Port of Environment.py _PositionedSchemeError and subclasses.
// C++ try/catch corresponds to Python's exception hierarchy.
// Every subclass carries an optional SourceInfo* for diagnostics.

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4275) // non-DLL-interface base (std::exception)
#endif

class CPPSCHEME2_API PositionedSchemeError : public std::exception
   {
 public:
   std::string msg;
   SourceInfo* src;
   void* call_stack = nullptr; // set by Evaluator (Phase 10)

   explicit PositionedSchemeError(std::string message, SourceInfo* source = nullptr);
   ~PositionedSchemeError() override;
   PositionedSchemeError(const PositionedSchemeError& o);
   PositionedSchemeError& operator=(const PositionedSchemeError& o);
   PositionedSchemeError(PositionedSchemeError&& o) noexcept;
   PositionedSchemeError& operator=(PositionedSchemeError&& o) noexcept;
   const char* what() const noexcept override;
   std::string str() const; // format_with_caret(msg, src) -- user-facing display
   };

class CPPSCHEME2_API SchemeArityError : public PositionedSchemeError
   {
 public:
   using PositionedSchemeError::PositionedSchemeError;
   };

class CPPSCHEME2_API SchemeUnboundError : public PositionedSchemeError
   {
 public:
   using PositionedSchemeError::PositionedSchemeError;
   };

class CPPSCHEME2_API SchemeTypeError : public PositionedSchemeError
   {
 public:
   using PositionedSchemeError::PositionedSchemeError;
   };

class CPPSCHEME2_API SchemeRaised : public PositionedSchemeError
   {
 public:
   Value value;
   bool continuable;

   SchemeRaised(Value val, SourceInfo* source = nullptr, bool cont = false);

 protected:
   // For SchemeFileError / SchemeUserError: they build msg themselves and
   // call PositionedSchemeError directly (port of Python's direct
   // _PositionedSchemeError.__init__ bypass in those subclass __init__s).
   SchemeRaised(std::string prebuilt_msg, Value val, SourceInfo* source, bool cont);
   };

class CPPSCHEME2_API SchemeRuntimeError : public PositionedSchemeError
   {
 public:
   using PositionedSchemeError::PositionedSchemeError;
   };

class CPPSCHEME2_API SchemeFileError : public SchemeRaised
   {
 public:
   explicit SchemeFileError(const std::string& message, SourceInfo* source = nullptr);
   };

class CPPSCHEME2_API SchemeUserError : public SchemeRaised
   {
 public:
   SchemeUserError(const std::string& message,
                   std::vector<Value> irritants,
                   SourceInfo* source = nullptr);
   };

#ifdef _MSC_VER
#pragma warning(pop)
#endif

// ── arity_mismatch_msg ────────────────────────────────────────────────────────
// Port of Environment.py arity_mismatch_msg.
// hi == -1  → at least lo expected (Python None)
// hi == lo  → exactly lo expected
// hi == lo+1 → lo or hi expected
// else       → lo to hi expected
CPPSCHEME2_API std::string arity_mismatch_msg(const std::string& name,
                                              int lo, int hi, int n_provided);

// ── Lexical-addressing scope classes ─────────────────────────────────────────
// Port of pyscheme/Environment.py.  Three classes:
//
//   Environment        abstract base: parent pointer, cached global
//                      pointer, and the virtual interface
//   LocalEnvironment   procedure call, let, let* step, letrec, named let:
//                      values in a vector
//   GlobalEnvironment  global / library, and the dict scopes chained under
//                      them (Expander macro scopes, (environment ...),
//                      make-environment): a name -> value hash map
//
// Every lookup / set / bind takes the same argument, an LA key (lexical
// addressing key) built by the Analyzer.  LocalEnvironment reads only
// depth and index; GlobalEnvironment reads only sid.  A global LA key
// (depth GLOBAL_DEPTH) met by a local scope is passed to the nearest dict
// scope above it, which falls through by name to its parents, so the
// Evaluator calls E->lookup(la_key) uniformly.
//
// By-name callers (the Expander, macro alias cells, aux-keyword checks,
// library / help / environment code) build a global LA key with
// global_la_key(sid): they only ever reach dict scopes, or -- for the
// aux-keyword check -- a name alpha-renaming guarantees no local carries.
// A name met in code running in a scope becomes an LA key with
// E->resolve(sid, 0), which answers exactly as the Analyzer would for code
// in that scope; the debugger uses it, and so does the Evaluator until the
// Analyzer emits LA keys itself.

constexpr int GLOBAL_DEPTH = -1;

// ── LA key ────────────────────────────────────────────────────────────────────
// Port of the Python (depth, index, name) tuple.  Built once by the
// Analyzer and stored in the variable-reference node; passed by const&.
struct LAKey
   {
   int depth;    // hops up the chain to the owning local scope; GLOBAL_DEPTH = a global
   int index;    // slot within that local scope
   uint32_t sid; // symbol id: the global's name, and the name for error messages
   };

// The LA key of a global variable: what the Analyzer emits for a name no
// local scope binds, and what by-name callers use.
inline LAKey global_la_key(uint32_t sid)
   {
   return LAKey{GLOBAL_DEPTH, 0, sid};
   }

// ScopeNames, the Analyzer's names for one local scope, is in AST.h (closures
// and the scope-names node hold them too).

// Callback for for_each_binding: (sid, value as stored).
using BindingVisitor = std::function<void(uint32_t, const Value&)>;

// ── GC placement ──────────────────────────────────────────────────────────────
// The GC reaches an object from its GcHeader with a cast, which needs the
// header at offset 0.  A class with virtual functions puts its vptr there,
// so the header lives in this non-polymorphic first base instead:
// &env->header is the header, and Environment::from_header undoes it with
// a static_cast down from this base (legal for a non-virtual base, and it
// applies the vptr offset).  So gc_gen.cpp never casts between GcHeader*
// and Environment* directly: it uses &env->header and from_header, and
// frees an Environment through its virtual destructor.
struct CPPSCHEME2_API EnvironmentGcBase
   {
   GcHeader header{GcType::Environment};
   };

// ── Environment ───────────────────────────────────────────────────────────────
// Abstract base.  The virtual interface takes the same arguments in both
// subclasses:
//   lookup(la_key)            value of the variable la_key names; throws
//                             SchemeUnboundError if it is unbound
//   lookup_optional(la_key)   that value, or nullopt if it is unbound
//   set(la_key, value)        update that variable
//   bind(la_key, value)       create or overwrite it in THIS scope
//   resolve(sid, depth)       the LA key the Analyzer would give sid in code
//                             running in this scope (depth counts the local
//                             scopes already passed; callers pass 0)
//   for_each_binding(fn)      call fn(sid, value) for each binding in THIS
//                             scope; values are passed as stored (an alias
//                             cell as the cell)
// plus the GC hooks behind the free functions gc_trace_environment_children
// / gc_forward_environment_children (gc.h), which gc_gen.cpp calls.

struct GlobalEnvironment;

struct CPPSCHEME2_API Environment : public EnvironmentGcBase
   {
public:
   virtual ~Environment() = default;

   virtual Value lookup(const LAKey& la_key) const = 0;
   virtual std::optional<Value> lookup_optional(const LAKey& la_key) const = 0;
   virtual Value set(const LAKey& la_key, Value value) = 0;
   virtual Value bind(const LAKey& la_key, Value value) = 0;
   virtual LAKey resolve(uint32_t sid, int depth) const = 0;
   virtual void for_each_binding(const BindingVisitor& fn) const = 0;

   virtual void gc_trace_children() = 0;
   virtual void gc_forward_children() = 0;

   GlobalEnvironment* getGlobalEnv() const;
   Environment* getParent() const;

   static Environment* from_header(GcHeader* header);

   Environment(const Environment&) = delete;
   Environment& operator=(const Environment&) = delete;

protected:
   explicit Environment(Environment* parent);

   Environment* _parent;
   Environment* _global_env;
   // The nearest dict scope at or above this one, where a local scope looks
   // up a global LA key.  It is the root global env unless code runs in a
   // dict scope chained under the root (make-environment, an Expander macro
   // scope): a variable defined there must still be found.
   Environment* _dict_env;
   };

// ── LocalEnvironment ──────────────────────────────────────────────────────────
// A local scope: one per procedure call, let, let* step, letrec and named
// let.  Its values sit in a vector at the indexes the Analyzer assigned;
// _names is the Analyzer's ScopeNames for the scope, used only by resolve
// and for_each_binding.  A local scope always has a parent, has a fixed
// size, is never frozen, and never holds an alias cell (those live in the
// global env).

struct CPPSCHEME2_API LocalEnvironment : public Environment
   {
public:
   LocalEnvironment(Environment* parent, ScopeNames names, std::vector<Value> values);

   Value lookup(const LAKey& la_key) const override;
   std::optional<Value> lookup_optional(const LAKey& la_key) const override;
   Value set(const LAKey& la_key, Value value) override;
   Value bind(const LAKey& la_key, Value value) override;
   LAKey resolve(uint32_t sid, int depth) const override;
   void for_each_binding(const BindingVisitor& fn) const override;

   void gc_trace_children() override;
   void gc_forward_children() override;

private:
   const LocalEnvironment* owner(int depth) const;
   LocalEnvironment* owner(int depth);

   ScopeNames _names;
   std::vector<Value> _values;
   };

// ── GlobalEnvironment ─────────────────────────────────────────────────────────
// Global / library scope: bindings in a name -> value hash map.  The root
// of every runtime chain.  Also used with a parent for the Expander's
// compile-time macro scopes and (environment ...) isolation scopes; that
// parent is always another GlobalEnvironment, and a name this scope does
// not hold is looked up there.

struct CPPSCHEME2_API GlobalEnvironment : public Environment
   {
public:
   explicit GlobalEnvironment(Environment* parent = nullptr);

   Value lookup(const LAKey& la_key) const override;
   std::optional<Value> lookup_optional(const LAKey& la_key) const override;
   Value set(const LAKey& la_key, Value value) override;
   Value bind(const LAKey& la_key, Value value) override;
   LAKey resolve(uint32_t sid, int depth) const override;
   void for_each_binding(const BindingVisitor& fn) const override;

   void gc_trace_children() override;
   void gc_forward_children() override;

   // Mark this environment immutable: subsequent bind / set on a binding it
   // owns raises SchemeTypeError.  Used for library export tables and for
   // environments returned by R7RS (environment ...).  Idempotent.
   void freeze();

   // Make this scope its own global root while keeping its parent chain for
   // lookups (make-toplevel-environment).
   void reroot_as_global();

   // Bind gs (a macro free-identifier gensym) in the global env to an alias
   // cell indirecting to target in def_env.
   void register_alias(uint32_t gs, uint32_t target, Environment* def_env,
                       Value copy_value);

private:
   std::unordered_map<uint32_t, Value> _bindings;
   bool _is_immutable;
   };

// ── GC allocators ─────────────────────────────────────────────────────────────
// Allocate on the heap, register with the GC young list.

CPPSCHEME2_API LocalEnvironment* gc_alloc_local_environment(Environment* parent, ScopeNames names,
                                             std::vector<Value> values);
CPPSCHEME2_API GlobalEnvironment* gc_alloc_global_environment(Environment* parent = nullptr);
