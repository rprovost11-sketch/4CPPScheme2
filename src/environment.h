#pragma once
// Environment.h -- lexical environment and Scheme runtime-error hierarchy.
// Direct port of pyscheme/Environment.py.
#include "gc.h"
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

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

// ── Environment ───────────────────────────────────────────────────────────────
// Port of Environment.py Environment class.
// GC-managed heap object; GcHeader must remain the first field.

struct CPPSCHEME2_API Environment
   {
   GcHeader header{GcType::Environment};
   std::unordered_map<uint32_t, Value> _bindings;
   Environment* _parent;
   Environment* _global_env;
   bool _is_immutable;

   explicit Environment(Environment* parent = nullptr,
                        std::unordered_map<std::string, Value> initial_bindings = {});

   Value bind(const std::string& key, Value value);
   Value bind_id(uint32_t sid, Value value);
   void freeze();
   Environment* getGlobalEnv() const;

   Value lookup(const std::string& key) const;
   Value lookup_id(uint32_t sid) const;
   std::optional<Value> lookup_optional(const std::string& key) const;
   std::optional<Value> lookup_optional_id(uint32_t sid) const;

   Value set(const std::string& key, Value value);
   Value set_id(uint32_t sid, Value value);

   Environment(const Environment&) = delete;
   Environment& operator=(const Environment&) = delete;
   };
