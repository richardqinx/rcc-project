# ─── Testbench CMake ──────────────────────────────────────────────────────────────
#
# Each testbench is registered as a Verilator simulation target + CTest.
#
# To add a new unit test, call:
#   add_verilator_target(
#       NAME        tb_<module>
#       TOP_MODULE  <module>
#       SOURCES     ${RTL_CORE_DIR}/<module>.sv  [... dependencies ...]
#       INCLUDE_DIRS ${RTL_INCLUDE_DIR}
#       TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_<module>.cpp
#   )
#

enable_testing()

# ─── Collect common RTL include dirs ─────────────────────────────────────────────
set(RTL_COMMON_INCLUDES
    ${RTL_INCLUDE_DIR}
)

# ─── Collect all RTL sources for SoC-level sim ───────────────────────────────────
file(GLOB_RECURSE ALL_RTL_SOURCES
    ${RTL_DIR}/*.sv
)

# ═══════════════════════════════════════════════════════════════════════════════════
# Unit Tests
# ═══════════════════════════════════════════════════════════════════════════════════
#
# Uncomment and fill in as you implement each module, for example:
#
add_verilator_target(
    NAME        tb_alu
    TOP_MODULE  alu
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/alu.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_alu.cc
)
#
# add_verilator_target(
#     NAME        tb_decoder
#     TOP_MODULE  decoder
#     SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
#                 ${RTL_CORE_DIR}/decoder.sv
#     INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
#     TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_decoder.cc
# )

add_verilator_target(
    NAME        tb_regfile
    TOP_MODULE  regfile
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/regfile.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_regfile.cc
)
#
# add_verilator_target(
#     NAME        tb_csr
#     TOP_MODULE  csr_unit
#     SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
#                 ${RTL_CORE_DIR}/include/csr_pkg.sv
#                 ${RTL_CORE_DIR}/csr_unit.sv
#     INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
#     TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_csr.cc
# )

# ═══════════════════════════════════════════════════════════════════════════════════
# Core Integration Test
# ═══════════════════════════════════════════════════════════════════════════════════
#
# add_verilator_target(
#     NAME        tb_core
#     TOP_MODULE  richard_core
#     SOURCES     ${ALL_RTL_SOURCES}
#     INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
#     TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/core/tb_core.cc
# )

# ═══════════════════════════════════════════════════════════════════════════════════
# SoC Integration Test
# ═══════════════════════════════════════════════════════════════════════════════════
#
# add_verilator_target(
#     NAME        tb_soc
#     TOP_MODULE  richard_soc
#     SOURCES     ${ALL_RTL_SOURCES}
#     INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
#     TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/soc/tb_soc.cc
#     EXTRA_FLAGS --threads 2
# )

# ═══════════════════════════════════════════════════════════════════════════════════
# Compliance Tests
# ═══════════════════════════════════════════════════════════════════════════════════
#
# add_subdirectory(compliance)
