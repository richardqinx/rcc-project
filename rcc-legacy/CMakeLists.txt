# Source code for RCC-Legacy.
# Copyright (C) 2026 Richard Qin
#
# This file is part of RCC.
#
# RCC is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free
# Software Foundation; either version 3, or (at your option) any later
# version.
#
# RCC is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.
#
# You should have received a copy of the GNU General Public License
# along with RCC; see the file LICENSE.  If not see
# <http://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.20)
project(rcc
    VERSION   0.1.0
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Export compile_commands.json for clangd / IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler Warnings
set(RCC_WARNING_FLAGS
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor
    -Wold-style-cast -Wcast-align -Wunused
    -Woverloaded-virtual -Wconversion -Wsign-conversion
    -Wnull-dereference -Wdouble-promotion
    -Wformat=2 -Wimplicit-fallthrough
)

# Sanitizers (Debug builds)
option(RCC_ENABLE_SANITIZERS "Enable ASan + UBSan in Debug builds" ON)
if(RCC_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# # Find Flex & Bison
find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)

# Generate lexer and parser
BISON_TARGET(SysYParser
    ${CMAKE_SOURCE_DIR}/src/frontend/parser.y
    ${CMAKE_CURRENT_BINARY_DIR}/parser.tab.cc
    DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/parser.tab.h
    COMPILE_FLAGS "--warnings=none"
)

FLEX_TARGET(SysYLexer
    ${CMAKE_SOURCE_DIR}/src/frontend/lexer.l
    ${CMAKE_CURRENT_BINARY_DIR}/lexer.yy.cc
)

ADD_FLEX_BISON_DEPENDENCY(SysYLexer SysYParser)

# Source Files
set(RCC_SOURCES
    # Frontend
    src/frontend/ast.cc
    # src/frontend/lexer.yy.cc
    # src/frontend/parser.tab.cc

    # IR
    src/util.cc
    # src/ir/ir.cc
    src/ir/irgen.cc
    src/ir/irgen_expr.cc
    src/ir/irgen_util.cc
    src/ir/print.cc

    # Mid-end
    src/mid/loopinfo.cc
    src/mid/cfg.cc
    src/mid/live.cc
    src/mid/ssa.cc
    src/mid/load.cc
    src/mid/alias.cc
    src/mid/mem.cc
    src/mid/fold.cc
    src/mid/copy.cc
    src/mid/gvn.cc
    src/mid/gcm.cc
    src/mid/ifopt.cc
    src/mid/divopt.cc
    src/mid/mulopt.cc
    src/mid/storemrg.cc
    src/mid/licm.cc
    src/mid/scalar_repl.cc
    src/mid/unroll.cc
    src/mid/tre.cc
    src/mid/lsr.cc
    src/mid/loop_idiom.cc
    src/mid/reduction_unroll.cc
    src/mid/loopswap.cc
    src/mid/unswitch.cc
    src/mid/sccp.cc
    src/mid/reassoc.cc
    src/mid/dce.cc
    src/mid/dead_loop.cc
    src/mid/inline.cc
    src/mid/abi.cc

    src/backend/simpl.cc
    src/backend/spill.cc
    src/backend/rega.cc
    src/backend/peephole.cc
    src/backend/emit.cc
    src/backend/rv64/targ.cc
    src/backend/rv64/abi.cc
    src/backend/rv64/isel.cc
    src/backend/rv64/emit.cc

)

# Core Library
add_library(rcc_core STATIC
    ${RCC_SOURCES}
    ${BISON_SysYParser_OUTPUTS}
    ${FLEX_SysYLexer_OUTPUTS}
)

target_include_directories(rcc_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}       # For generated parser.tab.h
    # ${PROJECT_SOURCE_DIR}/src/frontend  # For pre-generated parser.tab.h
)

# Suppress warnings on generated code
set_source_files_properties(
    ${BISON_SysYParser_OUTPUTS}
    ${FLEX_SysYLexer_OUTPUTS}
    src/frontend/lexer.yy.cc
    src/frontend/parser.tab.cc
    PROPERTIES COMPILE_FLAGS "-w"
)

target_compile_options(rcc_core PRIVATE ${RCC_WARNING_FLAGS})

# Main Executable
add_executable(rcc src/main.cc)
target_link_libraries(rcc PRIVATE rcc_core)
target_compile_options(rcc PRIVATE ${RCC_WARNING_FLAGS})

# clang-format
find_program(CLANG_FORMAT_EXE NAMES clang-format)
if(CLANG_FORMAT_EXE)
    file(GLOB_RECURSE ALL_CXX_SOURCES
        ${PROJECT_SOURCE_DIR}/src/*.cc
        ${PROJECT_SOURCE_DIR}/include/*.h
    )
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXE} -i ${ALL_CXX_SOURCES}
        COMMENT "Running clang-format on all source files"
        VERBATIM
    )
endif()

# Tests
option(RCC_BUILD_TESTS "Build tests" ON)
if(RCC_BUILD_TESTS)
    enable_testing()
    # add_subdirectory(tests)  # TODO: uncomment when tests are ready
endif()
