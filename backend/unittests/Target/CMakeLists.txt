# SPDX-License-Identifier: GPL-3.0-or-later

add_library(RCCBackendTestTarget TestTarget.cpp)
add_library(RCCBackend::TestTarget ALIAS RCCBackendTestTarget)

rcc_backend_configure_target(RCCBackendTestTarget)
target_include_directories(
  RCCBackendTestTarget
  PUBLIC "${RCC_BACKEND_SOURCE_DIR}/unittests"
)
target_link_libraries(RCCBackendTestTarget PUBLIC RCCBackend::CodeGen)

add_executable(RCCBackendDescriptionGeneratorTests DescriptionGeneratorTest.cpp)
rcc_backend_configure_target(RCCBackendDescriptionGeneratorTests)
target_link_libraries(
  RCCBackendDescriptionGeneratorTests
  PRIVATE RCCBackendDescriptionGeneratorCore
)
add_test(NAME RCCBackendDescriptionGeneratorTests COMMAND RCCBackendDescriptionGeneratorTests)

add_executable(RCCBackendRISCVTargetTests RISCVTargetTest.cpp)
rcc_backend_configure_target(RCCBackendRISCVTargetTests)
target_link_libraries(RCCBackendRISCVTargetTests PRIVATE RCCBackend::RISCV)
add_test(NAME RCCBackendRISCVTargetTests COMMAND RCCBackendRISCVTargetTests)

add_executable(RCCBackendRISCVCallLoweringTests RISCVCallLoweringTest.cpp)
rcc_backend_configure_target(RCCBackendRISCVCallLoweringTests)
target_link_libraries(RCCBackendRISCVCallLoweringTests PRIVATE RCCBackend::RISCV)
add_test(NAME RCCBackendRISCVCallLoweringTests COMMAND RCCBackendRISCVCallLoweringTests)

add_executable(RCCBackendRISCVCodeGenTests RISCVCodeGenTest.cpp)
rcc_backend_configure_target(RCCBackendRISCVCodeGenTests)
target_link_libraries(RCCBackendRISCVCodeGenTests PRIVATE RCCBackend::RISCV RCCBackend::CodeGen)
add_test(NAME RCCBackendRISCVCodeGenTests COMMAND RCCBackendRISCVCodeGenTests)

add_executable(RCCBackendRISCVSSAExitTests RISCVSSAExitTest.cpp)
rcc_backend_configure_target(RCCBackendRISCVSSAExitTests)
target_link_libraries(RCCBackendRISCVSSAExitTests PRIVATE RCCBackend::RISCV RCCBackend::CodeGen)
add_test(NAME RCCBackendRISCVSSAExitTests COMMAND RCCBackendRISCVSSAExitTests)

add_executable(RCCBackendRISCVRegisterAllocationTests RISCVRegisterAllocationTest.cpp)
rcc_backend_configure_target(RCCBackendRISCVRegisterAllocationTests)
target_link_libraries(
  RCCBackendRISCVRegisterAllocationTests
  PRIVATE RCCBackend::RISCV RCCBackend::CodeGen
)
add_test(
  NAME RCCBackendRISCVRegisterAllocationTests
  COMMAND RCCBackendRISCVRegisterAllocationTests
)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/DescriptionGeneratorTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVCallLoweringTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVTargetTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVCodeGenTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVSSAExitTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVRegisterAllocationTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TestTarget.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TestTarget.h"
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/DescriptionGeneratorTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVCallLoweringTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVTargetTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVCodeGenTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVSSAExitTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVRegisterAllocationTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TestTarget.cpp"
)
