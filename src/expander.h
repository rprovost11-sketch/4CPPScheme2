#pragma once
// Expander.h -- S-expression expander: rewrites sugar and user macros.
// Direct port of pyscheme/Expander.py.
#include "AST.h"
#include "Environment.h"
#include <string>

// Install/retrieve the runtime environment for macro lookup and define-syntax.
CEKSCHEME_API void         set_runtime_env(Environment* env);
CEKSCHEME_API Environment* get_runtime_env();

// Expand sugar and user macros in one S-expression.
CEKSCHEME_API Value expand(const Value& sexpr);

// Set/get the fallback directory used to resolve relative include paths from REPL input.
CEKSCHEME_API void        set_include_fallback_dir(const std::string& dir);
CEKSCHEME_API std::string get_include_fallback_dir();

// Resolve a cond-expand feature requirement against the platform feature set.
// Port of Expander.py _feature_req_matches.
CEKSCHEME_API bool feature_req_matches(const Value& req);

// Return the base directory to use for relative include paths.
// Port of Expander.py _include_base_dir.
CEKSCHEME_API std::string include_base_dir(SourceInfo* src);
