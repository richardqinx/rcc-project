# ─── Compliance Test CMake ───────────────────────────────────────────────────────
#
# Builds the compliance test runner and registers riscv-arch-test tests.
#
# ─────────────────────────────────────────────────────────────────────────────────

# The compliance runner needs the SoC sim binary
# Uncomment when both the SoC and compliance_runner are implemented:
#
# add_verilator_target(
#     NAME        compliance_runner
#     TOP_MODULE  richard_soc
#     SOURCES     ${ALL_RTL_SOURCES}
#     INCLUDE_DIRS ${RTL_COMMON_INCLUDES}
#     TB_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/compliance_runner.cpp
#     EXTRA_FLAGS --threads 2
# )
#
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
