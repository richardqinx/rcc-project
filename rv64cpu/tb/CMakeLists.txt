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

add_verilator_target(
    NAME        tb_decoder
    TOP_MODULE  decoder
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/decoder.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_decoder.cc
)

add_verilator_target(
    NAME        tb_regfile
    TOP_MODULE  regfile
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/regfile.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_regfile.cc
)

add_verilator_target(
    NAME        tb_branch
    TOP_MODULE  branch_unit
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/branch_unit.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_branch.cc
)

add_verilator_target(
    NAME        tb_if_stage
    TOP_MODULE  if_stage
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/if_stage.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_if_stage.cc
)

add_verilator_target(
    NAME        tb_id_stage
    TOP_MODULE  id_stage
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/decoder.sv
                ${RTL_CORE_DIR}/regfile.sv
                ${RTL_CORE_DIR}/id_stage.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_id_stage.cc
)

add_verilator_target(
    NAME        tb_if_id_reg
    TOP_MODULE  if_id_reg
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/if_id_reg.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_if_id_reg.cc
)

add_verilator_target(
    NAME        tb_id_ex_reg
    TOP_MODULE  id_ex_reg
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/id_ex_reg.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_id_ex_reg.cc
)

add_verilator_target(
    NAME        tb_ex_stage
    TOP_MODULE  ex_stage
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/alu.sv
                ${RTL_CORE_DIR}/branch_unit.sv
                ${RTL_CORE_DIR}/ex_stage.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_ex_stage.cc
)

add_verilator_target(
    NAME        tb_ex_mem_reg
    TOP_MODULE  ex_mem_reg
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/ex_mem_reg.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_ex_mem_reg.cc
)

add_verilator_target(
    NAME        tb_mem_stage
    TOP_MODULE  mem_stage
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/mem_stage.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_mem_stage.cc
)

add_verilator_target(
    NAME        tb_mem_wb_reg
    TOP_MODULE  mem_wb_reg
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/mem_wb_reg.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_mem_wb_reg.cc
)

add_verilator_target(
    NAME        tb_wb_stage
    TOP_MODULE  wb_stage
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/wb_stage.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_wb_stage.cc
)

add_verilator_target(
    NAME        tb_hazard_unit
    TOP_MODULE  hazard_unit
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/hazard_unit.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_hazard_unit.cc
)

add_verilator_target(
    NAME        tb_trap_ctrl
    TOP_MODULE  trap_ctrl
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/include/csr_pkg.sv
                ${RTL_CORE_DIR}/trap_ctrl.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_trap_ctrl.cc
)
#
add_verilator_target(
    NAME        tb_csr
    TOP_MODULE  csr_unit
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/include/csr_pkg.sv
                ${RTL_CORE_DIR}/csr_unit.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_csr.cc
)

add_verilator_target(
    NAME        tb_tlb
    TOP_MODULE  tlb
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/mmu/tlb.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/unit/tb_tlb.cc
)

# ═══════════════════════════════════════════════════════════════════════════════════
# Core Integration Test
# ═══════════════════════════════════════════════════════════════════════════════════

add_verilator_target(
    NAME        tb_core
    TOP_MODULE  richard_core
    SOURCES     ${RTL_CORE_DIR}/include/rv64_pkg.sv
                ${RTL_CORE_DIR}/alu.sv
                ${RTL_CORE_DIR}/branch_unit.sv
                ${RTL_CORE_DIR}/decoder.sv
                ${RTL_CORE_DIR}/regfile.sv
                ${RTL_CORE_DIR}/if_stage.sv
                ${RTL_CORE_DIR}/if_id_reg.sv
                ${RTL_CORE_DIR}/id_stage.sv
                ${RTL_CORE_DIR}/id_ex_reg.sv
                ${RTL_CORE_DIR}/ex_stage.sv
                ${RTL_CORE_DIR}/ex_mem_reg.sv
                ${RTL_CORE_DIR}/mem_stage.sv
                ${RTL_CORE_DIR}/mem_wb_reg.sv
                ${RTL_CORE_DIR}/wb_stage.sv
                ${RTL_CORE_DIR}/hazard_unit.sv
                ${RTL_CORE_DIR}/include/csr_pkg.sv
                ${RTL_CORE_DIR}/csr_unit.sv
                ${RTL_CORE_DIR}/trap_ctrl.sv
                ${RTL_CORE_DIR}/richard_core.sv
    INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
    TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/core/tb_core.cc
    EXTRA_FLAGS -Wno-PINCONNECTEMPTY
)

add_test(NAME tb_core_delegation COMMAND $<TARGET_FILE:tb_core> --case mret_sret_redirect)
add_test(NAME tb_core_interrupt_priority COMMAND $<TARGET_FILE:tb_core> --case branch_interrupt_priority)
add_test(NAME tb_core_double_exception COMMAND $<TARGET_FILE:tb_core> --case double_exception_flush)
add_test(NAME tb_core_mem_fault COMMAND $<TARGET_FILE:tb_core> --case mem_fault_pipeline)
add_test(NAME tb_core_mie_masking COMMAND $<TARGET_FILE:tb_core> --case mie_masking_response)

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

add_subdirectory(compliance)
