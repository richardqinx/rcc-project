# ─── Verilator CMake Helper ──────────────────────────────────────────────────────
#
# Provides the `add_verilator_target()` function to simplify creating
# Verilator simulation executables.
#
# Usage:
#   add_verilator_target(
#       NAME        tb_alu
#       TOP_MODULE  alu
#       SOURCES     ${RTL_CORE_DIR}/alu.sv
#       INCLUDE_DIRS ${RTL_INCLUDE_DIR}
#       TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_alu.cpp
#       PREFIX      Valu
#   )

function(add_verilator_target)
    cmake_parse_arguments(VT
        ""                                          # Options
        "NAME;TOP_MODULE;PREFIX"                     # Single-value args
        "SOURCES;INCLUDE_DIRS;TB_SOURCES;EXTRA_FLAGS" # Multi-value args
        ${ARGN}
    )

    if(NOT VT_PREFIX)
        set(VT_PREFIX "V${VT_TOP_MODULE}")
    endif()

    # Create C++ executable for the testbench
    add_executable(${VT_NAME} ${VT_TB_SOURCES})

    target_include_directories(${VT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/common
    )

    # Build include dir flags
    set(_INCLUDE_FLAGS "")
    foreach(_dir ${VT_INCLUDE_DIRS})
        list(APPEND _INCLUDE_FLAGS "-I${_dir}")
    endforeach()

    # Verilate the RTL sources
    verilate(${VT_NAME}
        PREFIX      ${VT_PREFIX}
        SOURCES     ${VT_SOURCES}
        TOP_MODULE  ${VT_TOP_MODULE}
        VERILATOR_ARGS
            ${VERILATOR_COMMON_FLAGS}
            ${_INCLUDE_FLAGS}
            ${VT_EXTRA_FLAGS}
    )

    # Register as CTest
    add_test(
        NAME    ${VT_NAME}
        COMMAND ${VT_NAME}
    )

    # Set test timeout
    set_tests_properties(${VT_NAME} PROPERTIES TIMEOUT 60)
endfunction()
