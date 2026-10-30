#pragma once
// Analyzer.h -- semantic validator and lexical addresser.
// Direct port of pyscheme/Analyzer.py.
#include "AST.h"
#include "Environment.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Static arity environment: name -> optional(lo, hi).
// hi == -1 means variadic (no upper bound).  nullopt means arity unknown
// (suppresses static arity checking for that name).
using StaticEnv = std::unordered_map<std::string, std::optional<std::pair<int, int>>>;

// ── SchemeAnalysisError ───────────────────────────────────────────────────────
// Port of Analyzer.py SchemeAnalysisError.

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4275)
#endif

class CPPSCHEME2_API SchemeAnalysisError : public PositionedSchemeError
   {
 public:
   using PositionedSchemeError::PositionedSchemeError;
   };

#ifdef _MSC_VER
#pragma warning(pop)
#endif

// ── Public API ────────────────────────────────────────────────────────────────
// Port of Analyzer.py analyze() and extend_static_env_with_define().

// A compile-time scope chain (port of the Python (names, outer) pair): one
// run-time local scope's names, in slot order, and the chain outside it.  A
// null chain means the code runs in a dict scope (top level, library, eval
// environment).
struct CompileScope
   {
   ScopeNames names;
   const CompileScope* outer = nullptr;
   };

// Validate sexpr and resolve its variable references against scope.  Returns
// the rewritten tree (see Analyzer.py "Lexical addressing"); the input is
// never edited.  Throws SchemeAnalysisError or SchemeArityError on failure.
// No-senv overload seeds from the registered primitive arities.
CPPSCHEME2_API Value analyze(const Value& sexpr);
CPPSCHEME2_API Value analyze(const Value& sexpr, const StaticEnv& senv,
                             const CompileScope* scope = nullptr);

// The scope chain for code that will run in the run-time env env, read off
// the live chain up to the nearest dict scope; links owns it.  Returns its
// innermost scope, or nullptr when env is itself a dict scope.
CPPSCHEME2_API const CompileScope* scope_of_env(Environment* env,
                                                std::vector<CompileScope>& links);

// Update senv from a top-level (define name value) form.
CPPSCHEME2_API void extend_static_env_with_define(StaticEnv& senv, const Value& sexpr);

// Register a primitive arity (called from Phase 11 primitives).
// hi == -1 means variadic.
CPPSCHEME2_API void register_primitive_arity(const std::string& name, int lo, int hi);

// Access the currently registered primitive arities.
CPPSCHEME2_API const StaticEnv& primitive_arities();
