# SPDX-License-Identifier: GPL-3.0-or-later

include_directories("${RCC_BACKEND_SOURCE_DIR}/lib/Target/RISCV")

add_library(RCCBackendTestTarget TestTarget.cpp)
add_library(RCCBackend::TestTarget ALIAS RCCBackendTestTarget)

rcc_backend_configure_target(RCCBackendTestTarget)
target_include_directories(
  RCCBackendTestTarget
  PUBLIC "${RCC_BACKEND_SOURCE_DIR}/unittests"
)
target_link_libraries(RCCBackendTestTarget PUBLIC RCCBackend::CodeGen)

add_executable(RCCBackendTableGenTests TableGenTest.cpp)
rcc_backend_configure_target(RCCBackendTableGenTests)
target_link_libraries(RCCBackendTableGenTests PRIVATE RCCBackend::TableGenBackends)
add_test(NAME RCCBackend.TableGen COMMAND RCCBackendTableGenTests)

add_executable(RCCBackendRISCVTargetTests RISCVTargetTest.cpp)
rcc_backend_configure_target(RCCBackendRISCVTargetTests)
target_link_libraries(RCCBackendRISCVTargetTests PRIVATE RCCBackend::RISCV)
add_test(NAME RCCBackendRISCVTargetTests COMMAND RCCBackendRISCVTargetTests)

add_executable(RCCBackendRISCVCallLoweringTests RISCVCallLoweringTest.cpp)
rcc_backend_configure_target(RCCBackendRISCVCallLoweringTests)
target_link_libraries(RCCBackendRISCVCallLoweringTests PRIVATE RCCBackend::RISCV)
add_test(NAME RCCBackendRISCVCallLoweringTests COMMAND RCCBackendRISCVCallLoweringTests)

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

add_executable(RCCBackendRISCVFrameLoweringTests RISCVFrameLoweringTest.cpp)
rcc_backend_configure_target(RCCBackendRISCVFrameLoweringTests)
target_link_libraries(
  RCCBackendRISCVFrameLoweringTests
  PRIVATE RCCBackend::RISCV RCCBackend::CodeGen
)
add_test(
  NAME RCCBackendRISCVFrameLoweringTests
  COMMAND RCCBackendRISCVFrameLoweringTests
)

add_executable(RCCBackendRISCVLateCodeGenTests RISCVLateCodeGenTest.cpp)
rcc_backend_configure_target(RCCBackendRISCVLateCodeGenTests)
target_link_libraries(
  RCCBackendRISCVLateCodeGenTests
  PRIVATE RCCBackend::RISCV RCCBackend::CodeGen
)
add_test(
  NAME RCCBackendRISCVLateCodeGenTests
  COMMAND RCCBackendRISCVLateCodeGenTests
)

add_executable(RCCBackendRISCVAssemblyPrinterTests RISCVAssemblyPrinterTest.cpp)
rcc_backend_configure_target(RCCBackendRISCVAssemblyPrinterTests)
target_link_libraries(
  RCCBackendRISCVAssemblyPrinterTests
  PRIVATE RCCBackend::RISCV RCCBackend::CodeGen
)
add_test(
  NAME RCCBackendRISCVAssemblyPrinterTests
  COMMAND RCCBackendRISCVAssemblyPrinterTests
)

add_executable(RCCBackendRISCVMCTests RISCVMCTest.cpp)
rcc_backend_configure_target(RCCBackendRISCVMCTests)
target_link_libraries(RCCBackendRISCVMCTests PRIVATE RCCBackend::RISCV RCCBackend::MC)
add_test(NAME RCCBackendRISCVMCTests COMMAND RCCBackendRISCVMCTests)

add_executable(RCCBackendRISCVMCEncodingCoverageTests RISCVMCEncodingCoverageTest.cpp)
rcc_backend_configure_target(RCCBackendRISCVMCEncodingCoverageTests)
target_link_libraries(
  RCCBackendRISCVMCEncodingCoverageTests
  PRIVATE RCCBackend::RISCV RCCBackend::MC
)
add_test(
  NAME RCCBackendRISCVMCEncodingCoverageTests
  COMMAND RCCBackendRISCVMCEncodingCoverageTests
)

add_executable(RCCBackendRISCVObjectFileTests RISCVObjectFileTest.cpp)
rcc_backend_configure_target(RCCBackendRISCVObjectFileTests)
target_link_libraries(
  RCCBackendRISCVObjectFileTests
  PRIVATE RCCBackend::RISCV RCCBackend::MC RCCBackend::CodeGen
)
add_test(NAME RCCBackendRISCVObjectFileTests COMMAND RCCBackendRISCVObjectFileTests)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/TableGenTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVCallLoweringTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVTargetTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVSSAExitTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVRegisterAllocationTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVFrameLoweringTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVLateCodeGenTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVAssemblyPrinterTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMCTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMCEncodingCoverageTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVObjectFileTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TestTarget.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TestTarget.h"
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/TableGenTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVCallLoweringTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVTargetTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVSSAExitTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVRegisterAllocationTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVFrameLoweringTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVLateCodeGenTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVAssemblyPrinterTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMCTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMCEncodingCoverageTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVObjectFileTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TestTarget.cpp"
)
