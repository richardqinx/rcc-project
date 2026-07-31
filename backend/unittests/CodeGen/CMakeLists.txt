# SPDX-License-Identifier: GPL-3.0-or-later

set(
  RCCBackendCodeGenTestSources
  CoreTypesTest.cpp
  MachineIRCoreTest.cpp
  MachineValueTypesTest.cpp
)

add_executable(RCCBackendCodeGenCoreTypesTests CoreTypesTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenCoreTypesTests)
target_link_libraries(RCCBackendCodeGenCoreTypesTests PRIVATE RCCBackend::CodeGen)

add_test(NAME RCCBackend.CodeGen.CoreTypes COMMAND RCCBackendCodeGenCoreTypesTests)

add_executable(RCCBackendCodeGenMachineValueTypesTests MachineValueTypesTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenMachineValueTypesTests)
target_link_libraries(RCCBackendCodeGenMachineValueTypesTests PRIVATE RCCBackend::CodeGen)

add_test(
  NAME RCCBackend.CodeGen.MachineValueTypes
  COMMAND RCCBackendCodeGenMachineValueTypesTests
)

add_executable(RCCBackendCodeGenMachineIRCoreTests MachineIRCoreTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenMachineIRCoreTests)
target_link_libraries(RCCBackendCodeGenMachineIRCoreTests PRIVATE RCCBackend::CodeGen)

add_test(
  NAME RCCBackend.CodeGen.MachineIRCore
  COMMAND RCCBackendCodeGenMachineIRCoreTests
)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/CoreTypesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineIRCoreTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineValueTypesTest.cpp"
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/CoreTypesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineIRCoreTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineValueTypesTest.cpp"
)
