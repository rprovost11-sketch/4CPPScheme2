#pragma once
#include "scheme_export.h"
#include <string>
#include <string_view>
#include <cstdint>

// Integer intern pool: maps name strings to stable uint32_t IDs.
// ID equality implies name equality; comparison is O(1).
// IDs are assigned in insertion order starting at 0.
// Symbols never expire — they live for the process lifetime.

SCHEME_API uint32_t            intern_symbol(std::string_view name);
SCHEME_API const std::string&  symbol_name(uint32_t sid);

// Wipe the intern table — useful in unit tests.
SCHEME_API void                intern_table_clear();
