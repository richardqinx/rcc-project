cmake_minimum_required(VERSION 3.20)
project(richard_cpu
    VERSION 0.1.0
    DESCRIPTION "RV64IMAZicsr_Zifencei Five-Stage Pipelined CPU SoC"
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Options ────────────────────────────────────────────────────────────────────
option(ENABLE_TRACE     "Enable VCD waveform tracing"       ON)
option(ENABLE_COVERAGE  "Enable code coverage for RTL"      OFF)
option(BUILD_SW         "Build software targets (requires riscv64 toolchain)" OFF)

# ─── Locate Verilator ────────────────────────────────────────────────────────────
find_package(verilator REQUIRED HINTS $ENV{VERILATOR_ROOT})
message(STATUS "Found Verilator: ${verilator_DIR}")

# ─── RTL Source Directories ──────────────────────────────────────────────────────
set(RTL_DIR         ${CMAKE_SOURCE_DIR}/rtl)
set(RTL_CORE_DIR    ${RTL_DIR}/core)
set(RTL_CACHE_DIR   ${RTL_DIR}/cache)
set(RTL_BUS_DIR     ${RTL_DIR}/bus)
set(RTL_PERIPS_DIR  ${RTL_DIR}/perips)
set(RTL_SOC_DIR     ${RTL_DIR}/soc)
set(RTL_INCLUDE_DIR ${RTL_CORE_DIR}/include)

# ─── Common Verilator Flags ──────────────────────────────────────────────────────
set(VERILATOR_COMMON_FLAGS
    --sv
    -Wall
    -Wno-DECLFILENAME
    -Wno-UNUSED
    --x-initial unique
    --x-assign unique
)

if(ENABLE_TRACE)
    list(APPEND VERILATOR_COMMON_FLAGS --trace)
    add_compile_definitions(VM_TRACE=1)
endif()

if(ENABLE_COVERAGE)
    list(APPEND VERILATOR_COMMON_FLAGS --coverage)
endif()

# ─── Include Verilator CMake Helper ──────────────────────────────────────────────
include(${CMAKE_SOURCE_DIR}/tools/verilator.cmake)

# ─── Waveform Output Directory ──────────────────────────────────────────────────
file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/waves)

# ─── Testing ────────────────────────────────────────────────────────────────────
enable_testing()

# ─── Testbench Subdirectory ─────────────────────────────────────────────────────
add_subdirectory(tb)

# ─── Software Subdirectory (optional) ───────────────────────────────────────────
if(BUILD_SW)
    add_subdirectory(sw)
endif()

# ─── Summary ────────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "╔══════════════════════════════════════════════╗")
message(STATUS "║  richard-cpu build configuration             ║")
message(STATUS "╠══════════════════════════════════════════════╣")
message(STATUS "║  Trace:     ${ENABLE_TRACE}")
message(STATUS "║  Coverage:  ${ENABLE_COVERAGE}")
message(STATUS "║  Software:  ${BUILD_SW}")
message(STATUS "╚══════════════════════════════════════════════╝")
message(STATUS "")
