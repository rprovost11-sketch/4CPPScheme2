#pragma once
// library.h -- R7RS library system (5.6): define-library, import, export.
// Direct port of pyscheme/library.py.
#include "AST.h"
#include "Environment.h"
#include "scheme_export.h"
#include <string>
#include <unordered_map>

CEKSCHEME_API std::string  library_name_to_key(const Value& name_sexpr);
CEKSCHEME_API void         library_register(const std::string& key, Environment* exports_env);
CEKSCHEME_API Environment* library_lookup(const std::string& key);
CEKSCHEME_API bool         library_registered_p(const std::string& key);

CEKSCHEME_API std::unordered_map<std::string, Value>
    resolve_import_set(const Value& import_set);

CEKSCHEME_API void register_standard_libraries(Environment* global_env);
