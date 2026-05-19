cmake_minimum_required(VERSION 3.16)
project(cekscheme CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Core shared library ───────────────────────────────────────────────────────
add_library(cekscheme_core SHARED
    src/symbol.cpp
    src/value.cpp
    src/gc_gen.cpp
    src/environment.cpp
    src/parser.cpp
    src/expander.cpp
    src/syntax_rules.cpp
    src/analyzer.cpp
    src/evaluator_stubs.cpp
)

target_compile_definitions(cekscheme_core PRIVATE CEKSCHEME_CORE_EXPORTS)
target_include_directories(cekscheme_core PUBLIC src)

if(MSVC)
    target_compile_options(cekscheme_core PRIVATE /W4 /utf-8)
    target_compile_options(cekscheme_core PRIVATE /wd4100 /wd4458 /wd4251 /wd4275)
    target_compile_definitions(cekscheme_core PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(cekscheme_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_options(cekscheme_core PRIVATE -fvisibility=hidden)
endif()

# ── Main executable ───────────────────────────────────────────────────────────
add_executable(cekscheme src/main.cpp)
target_link_libraries(cekscheme PRIVATE cekscheme_core)

if(MSVC)
    target_compile_options(cekscheme PRIVATE /W4 /utf-8 /wd4100 /wd4458 /wd4251 /wd4275)
    target_compile_definitions(cekscheme PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(cekscheme PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Place DLL next to the executable.
set_target_properties(cekscheme_core PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
foreach(cfg IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${cfg}" CFG_UPPER)
    set_target_properties(cekscheme_core PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${CFG_UPPER} "${CMAKE_BINARY_DIR}/${cfg}"
        LIBRARY_OUTPUT_DIRECTORY_${CFG_UPPER} "${CMAKE_BINARY_DIR}/${cfg}"
        ARCHIVE_OUTPUT_DIRECTORY_${CFG_UPPER} "${CMAKE_BINARY_DIR}/${cfg}"
    )
endforeach()
