cmake_minimum_required(VERSION 3.20)
project(atp_engine
    VERSION   0.1.0
    LANGUAGES CXX
)

# ─── C++ Standard ────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clang-tidy and IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Compiler Warnings ──────────────────────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor
    -Wold-style-cast -Wcast-align -Wunused
    -Woverloaded-virtual -Wconversion -Wsign-conversion
    -Wnull-dereference -Wdouble-promotion
    -Wformat=2 -Wimplicit-fallthrough
)

# ─── Sanitizers (Debug builds) ───────────────────────────────────────────────
option(ATP_ENABLE_SANITIZERS "Enable ASan + UBSan in Debug builds" ON)
if(ATP_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ─── clang-tidy Integration ─────────────────────────────────────────────────
option(ATP_ENABLE_CLANG_TIDY "Enable clang-tidy static analysis" OFF)
if(ATP_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
        message(STATUS "clang-tidy found: ${CLANG_TIDY_EXE}")
    else()
        message(WARNING "clang-tidy requested but not found")
    endif()
endif()

# ─── clang-format Custom Target ─────────────────────────────────────────────
find_program(CLANG_FORMAT_EXE NAMES clang-format)
if(CLANG_FORMAT_EXE)
    file(GLOB_RECURSE ALL_CXX_SOURCES
        ${PROJECT_SOURCE_DIR}/src/*.cc
        ${PROJECT_SOURCE_DIR}/include/*.h
        ${PROJECT_SOURCE_DIR}/tests/*.cc
    )
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXE} -i ${ALL_CXX_SOURCES}
        COMMENT "Running clang-format on all source files"
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${ALL_CXX_SOURCES}
        COMMENT "Checking clang-format compliance"
        VERBATIM
    )
    message(STATUS "clang-format found: ${CLANG_FORMAT_EXE}")
    message(STATUS "  Use 'cmake --build . --target format'       to auto-format")
    message(STATUS "  Use 'cmake --build . --target format-check' to check format")
else()
    message(WARNING "clang-format not found, format targets disabled")
endif()

# ─── Source Files ────────────────────────────────────────────────────────────
set(ATP_SOURCES
    src/core/symbol_table.cc
    src/core/term_bank.cc
    src/core/clause_store.cc
    src/normalize/clausifier.cc
    src/infer/substitution.cc
    src/infer/unification.cc
    src/infer/resolution.cc
    src/index/discrim_tree.cc
    src/simplify/subsumption.cc
    src/search/prover.cc
    src/proof/proof_trace.cc
    src/rewrite/ordering.cc
    src/rewrite/paramodulation.cc
    src/strategy/problem_analyzer.cc
    src/frontend/tptp_parser.cc
)

# ─── Core Library ────────────────────────────────────────────────────────────
add_library(atp_core STATIC ${ATP_SOURCES})
target_include_directories(atp_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# ─── Main Executable ────────────────────────────────────────────────────────
add_executable(atp src/main.cc)
target_link_libraries(atp PRIVATE atp_core)

# ─── Tests ───────────────────────────────────────────────────────────────────
option(ATP_BUILD_TESTS "Build unit tests" ON)
if(ATP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
