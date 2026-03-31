# ─── Compliance Test CMake ───────────────────────────────────────────────────────
#
# Builds the compliance test runner and registers riscv-arch-test tests.
#
# ─────────────────────────────────────────────────────────────────────────────────

# Minimal compliance runner target (core-only for bootstrap stage).
# NOTE: This uses richard_core until SoC top and memory/peripheral harness are ready.
add_verilator_target(
	NAME        compliance_runner
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
	TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/compliance_runner.cc
	EXTRA_FLAGS -Wno-PINCONNECTEMPTY
)

# ─── Register individual compliance tests via CTest ──────────────────────────────
# This is done dynamically by the compliance_runner binary and run_compliance.py.
# You can also manually register tests like:
#
# set(RISCV_ARCH_TEST_DIR ${CMAKE_SOURCE_DIR}/sw/riscv-arch-test)
#
# file(GLOB RV64I_TESTS ${RISCV_ARCH_TEST_DIR}/riscv-test-suite/rv64i_m/I/src/*.S)
# foreach(test_src ${RV64I_TESTS})
#     get_filename_component(test_name ${test_src} NAME_WE)
#     add_test(
#         NAME compliance_rv64i_${test_name}
#         COMMAND compliance_runner --test ${test_src}
#     )
#     set_tests_properties(compliance_rv64i_${test_name} PROPERTIES
#         LABELS "compliance;rv64i"
#         TIMEOUT 30
#     )
# endforeach()
