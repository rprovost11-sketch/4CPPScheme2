cmake_minimum_required(VERSION 3.16)
project(cppscheme2 C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Core shared library ───────────────────────────────────────────────────────
add_library(cppscheme2_core SHARED
    # Vendored mini-gmp (compiled as C)
    src/mini-gmp/mini-gmp.c
    # Phase 1 undercarriage
    src/AST.cpp
    # Generated Unicode case/classification tables (gen_unicode_tables.py)
    src/unicode_tables.cpp
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
    src/tco_calibrate.cpp
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

target_compile_definitions(cppscheme2_core PRIVATE CPPSCHEME2_CORE_EXPORTS)
target_include_directories(cppscheme2_core PUBLIC src)

if(MSVC)
    target_compile_options(cppscheme2_core PRIVATE /W4 /utf-8)
    target_compile_options(cppscheme2_core PRIVATE /wd4100 /wd4458 /wd4251 /wd4275)
    target_compile_definitions(cppscheme2_core PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(ENABLE_ASAN)
        target_compile_options(cppscheme2_core PRIVATE /fsanitize=address)
        target_link_options(cppscheme2_core PRIVATE /fsanitize=address)
    endif()
else()
    target_compile_options(cppscheme2_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_options(cppscheme2_core PRIVATE -fvisibility=hidden)
    if(ENABLE_ASAN)
        target_compile_options(cppscheme2_core PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(cppscheme2_core PRIVATE -fsanitize=address)
    endif()
endif()

# ── Main executable ───────────────────────────────────────────────────────────
add_executable(cppscheme2 src/main.cpp)
target_link_libraries(cppscheme2 PRIVATE cppscheme2_core)

if(MSVC)
    target_compile_options(cppscheme2 PRIVATE /W4 /utf-8 /wd4100 /wd4458 /wd4251 /wd4275)
    target_compile_definitions(cppscheme2 PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(cppscheme2 PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ── GC regression test binary ─────────────────────────────────────────────────
add_executable(gc_test undercarriage-tests/gc_test.cpp)
target_link_libraries(gc_test PRIVATE cppscheme2_core)
if(MSVC)
    target_compile_options(gc_test PRIVATE /W4 /utf-8 /wd4100 /wd4458 /wd4251 /wd4275)
    target_compile_definitions(gc_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(ENABLE_ASAN)
        target_compile_options(gc_test PRIVATE /fsanitize=address)
        target_link_options(gc_test PRIVATE /fsanitize=address)
    endif()
else()
    target_compile_options(gc_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
    if(ENABLE_ASAN)
        target_compile_options(gc_test PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(gc_test PRIVATE -fsanitize=address)
    endif()
endif()

# ── Numeric-tower white-box unit test binary ──────────────────────────────────
# mini-gmp.c is compiled in directly: the vendored mpz_* C functions are used by
# the core DLL but not exported from it, and this white-box test drives them
# directly (mpz_init/set_str/clear on its own locals).  /W0 is inherited from the
# source-file property set above.
add_executable(rat_test undercarriage-tests/rat_test.cpp src/mini-gmp/mini-gmp.c)
target_link_libraries(rat_test PRIVATE cppscheme2_core)
if(MSVC)
    target_compile_options(rat_test PRIVATE /W4 /utf-8 /wd4100 /wd4458 /wd4251 /wd4275)
    target_compile_definitions(rat_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(ENABLE_ASAN)
        target_compile_options(rat_test PRIVATE /fsanitize=address)
        target_link_options(rat_test PRIVATE /fsanitize=address)
    endif()
else()
    target_compile_options(rat_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
    if(ENABLE_ASAN)
        target_compile_options(rat_test PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(rat_test PRIVATE -fsanitize=address)
    endif()
endif()

# ── Example native plugin (demonstrates the plugin ABI; loaded via import) ────
add_library(example_plugin SHARED examples/example_plugin.cpp)
target_link_libraries(example_plugin PRIVATE cppscheme2_core)
set_target_properties(example_plugin PROPERTIES PREFIX "")
if(MSVC)
    target_compile_options(example_plugin PRIVATE /W4 /utf-8 /wd4100 /wd4458 /wd4251 /wd4275)
    target_compile_definitions(example_plugin PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(example_plugin PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Place DLL next to the executable.
set_target_properties(cppscheme2_core PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
foreach(cfg IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${cfg}" CFG_UPPER)
    set_target_properties(cppscheme2_core PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${CFG_UPPER} "${CMAKE_BINARY_DIR}/${cfg}"
        LIBRARY_OUTPUT_DIRECTORY_${CFG_UPPER} "${CMAKE_BINARY_DIR}/${cfg}"
        ARCHIVE_OUTPUT_DIRECTORY_${CFG_UPPER} "${CMAKE_BINARY_DIR}/${cfg}"
    )
endforeach()
