set(RV64CPU_RTL_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/packages/rv64_pkg.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/packages/csr_pkg.sv

    ${CMAKE_CURRENT_SOURCE_DIR}/core/execute/integer_alu.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/execute/branch_unit.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/execute/instruction_decoder.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/execute/compressed_decoder.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/execute/hazard_unit.sv

    ${CMAKE_CURRENT_SOURCE_DIR}/core/registers/gpr_file.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/registers/fpr_file.sv

    ${CMAKE_CURRENT_SOURCE_DIR}/core/pipeline/if_stage.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/pipeline/if_id_reg.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/pipeline/id_stage.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/pipeline/id_ex_reg.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/pipeline/ex_stage.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/pipeline/ex_mem_reg.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/pipeline/mem_stage.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/pipeline/mem_wb_reg.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/pipeline/wb_stage.sv

    ${CMAKE_CURRENT_SOURCE_DIR}/core/privileged/csr_unit.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/privileged/trap_controller.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/privileged/pmp_checker.sv

    ${CMAKE_CURRENT_SOURCE_DIR}/core/mmu/tlb.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/mmu/ptw.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/mmu/mmu.sv

    ${CMAKE_CURRENT_SOURCE_DIR}/core/fpu/fpu.sv
    ${CMAKE_CURRENT_SOURCE_DIR}/core/rv64_core.sv
)

set(RV64CPU_RTL_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/packages
)

list(LENGTH RV64CPU_RTL_SOURCES RV64CPU_RTL_SOURCE_COUNT)

set(RV64CPU_RTL_SOURCES ${RV64CPU_RTL_SOURCES} PARENT_SCOPE)
set(RV64CPU_RTL_INCLUDE_DIRS ${RV64CPU_RTL_INCLUDE_DIRS} PARENT_SCOPE)
set(RV64CPU_RTL_SOURCE_COUNT ${RV64CPU_RTL_SOURCE_COUNT} PARENT_SCOPE)
