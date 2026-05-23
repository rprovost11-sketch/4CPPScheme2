cmake_minimum_required(VERSION 3.16)
project(cekscheme C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Core shared library ───────────────────────────────────────────────────────
add_library(cekscheme_core SHARED
    # Vendored mini-gmp (compiled as C)
    src/mini-gmp/mini-gmp.c
    # Phase 1 undercarriage
    src/AST.cpp
    src/gc_gen.cpp
    # Phase 2
    src/Environment.cpp
    # Phase 3
    src/Utils.cpp
    # Phase 4
    src/rational.cpp
    # Phase 5
    src/PrettyPrinter.cpp
    # Phase 6
    src/Parser.cpp
    # Phase 7
    src/syntax_rules.cpp
    # Phase 8
    src/Expander.cpp
    # Phase 9
    src/Analyzer.cpp
    # Phase 10
    src/Evaluator.cpp
    # Phase 11 primitives
    src/primitives/primitives.cpp
    src/primitives/arithmetic.cpp
    src/primitives/binding.cpp
    src/primitives/bytevectors.cpp
    src/primitives/chars.cpp
    src/primitives/comparison.cpp
    src/primitives/control.cpp
    src/primitives/debug.cpp
    src/primitives/equivalence.cpp
    src/primitives/help_sys.cpp
    src/primitives/lazy.cpp
    src/primitives/lists.cpp
    src/primitives/logical.cpp
    src/primitives/macros.cpp
    src/primitives/meta.cpp
    src/primitives/modules.cpp
    src/primitives/ports.cpp
    src/primitives/predicates.cpp
    src/primitives/quotation.cpp
    src/primitives/strings.cpp
    src/primitives/vectors.cpp
    # Phase 12
    src/Tracer.cpp
    # Phase 13
    src/library.cpp
    # Phase 14
    src/Context.cpp
    # Phase 15
    src/readline_win.cpp
    src/Listener.cpp
    # Phase 16
    src/Interpreter.cpp
    # Phase 17
    src/Debugger.cpp
    # Phase 18
    src/profiler.cpp
    # Phase 19
    src/plugin_loader.cpp
    src/plugin_api_impl.cpp
)

# Suppress warnings from vendored mini-gmp C source.
if(MSVC)
    set_source_files_properties(src/mini-gmp/mini-gmp.c PROPERTIES COMPILE_FLAGS "/W0")
else()
    set_source_files_properties(src/mini-gmp/mini-gmp.c PROPERTIES COMPILE_FLAGS "-w")
endif()

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
