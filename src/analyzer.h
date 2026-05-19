#pragma once
#include "value.h"
#include "scheme_export.h"
#include <unordered_map>
#include <string>
#include <utility>

// Static arity environment: name -> (min, max).
// max == -1 means variadic (at least min args).
// Absent key means arity unknown -> no static check.
using StaticEnv = std::unordered_map<std::string, std::pair<int,int>>;

// Validate a fully-expanded sexpr.  Returns val unchanged on success.
// Raises SchemeAnalysisError / SchemeArityError on failure.
// If static_env is nullptr a fresh env seeded with registered primitives is used.
SCHEME_API Value analyze(Value val, StaticEnv* static_env = nullptr);

// Mutate static_env from a top-level (define name value) form.
SCHEME_API void extend_static_env_with_define(StaticEnv& static_env, Value sexpr);

// Register a primitive arity so the default static_env includes it.
// Called from the primitives module once it exists.
SCHEME_API void register_primitive_arity(const std::string& name, int min_args, int max_args);

// Copy all registered primitive arities into static_env.
SCHEME_API void seed_static_env(StaticEnv& static_env);
