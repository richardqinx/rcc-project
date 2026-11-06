cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

project(rv64cpu
    VERSION 0.1.0
    DESCRIPTION "RV64 RISC-V processor core written in SystemVerilog"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_TRACE    "Enable VCD waveform tracing" OFF)
option(ENABLE_COVERAGE "Enable RTL code coverage" OFF)

find_package(verilator REQUIRED HINTS $ENV{VERILATOR_ROOT})
message(STATUS "Found Verilator: ${verilator_DIR}")

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

include(${CMAKE_SOURCE_DIR}/cmake/verilator.cmake)

add_subdirectory(rtl)

message(STATUS "")
message(STATUS "RV64CPU configuration")
message(STATUS "  Trace:    ${ENABLE_TRACE}")
message(STATUS "  Coverage: ${ENABLE_COVERAGE}")
message(STATUS "  RTL files: ${RV64CPU_RTL_SOURCE_COUNT}")
message(STATUS "")
