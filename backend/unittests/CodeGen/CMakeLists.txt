# SPDX-License-Identifier: GPL-3.0-or-later

set(
  RCCBackendCodeGenTestSources
  CoreTypesTest.cpp
  InstructionDescriptorTest.cpp
  MachineIRBuilderTest.cpp
  MachineIRCoreTest.cpp
  MachineVerifierTest.cpp
  MachineValueTypesTest.cpp
)

add_executable(RCCBackendCodeGenCoreTypesTests CoreTypesTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenCoreTypesTests)
target_link_libraries(RCCBackendCodeGenCoreTypesTests PRIVATE RCCBackend::CodeGen)

add_test(NAME RCCBackend.CodeGen.CoreTypes COMMAND RCCBackendCodeGenCoreTypesTests)

add_executable(
  RCCBackendCodeGenInstructionDescriptorTests
  InstructionDescriptorTest.cpp
)
rcc_backend_configure_target(RCCBackendCodeGenInstructionDescriptorTests)
target_link_libraries(
  RCCBackendCodeGenInstructionDescriptorTests
  PRIVATE RCCBackend::CodeGen
)

add_test(
  NAME RCCBackend.CodeGen.InstructionDescriptor
  COMMAND RCCBackendCodeGenInstructionDescriptorTests
)

add_executable(RCCBackendCodeGenMachineIRBuilderTests MachineIRBuilderTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenMachineIRBuilderTests)
target_link_libraries(
  RCCBackendCodeGenMachineIRBuilderTests
  PRIVATE RCCBackend::CodeGen
)

add_test(
  NAME RCCBackend.CodeGen.MachineIRBuilder
  COMMAND RCCBackendCodeGenMachineIRBuilderTests
)

add_executable(RCCBackendCodeGenMachineVerifierTests MachineVerifierTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenMachineVerifierTests)
target_link_libraries(
  RCCBackendCodeGenMachineVerifierTests
  PRIVATE RCCBackend::CodeGen
)

add_test(
  NAME RCCBackend.CodeGen.MachineVerifier
  COMMAND RCCBackendCodeGenMachineVerifierTests
)

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
  "${CMAKE_CURRENT_SOURCE_DIR}/InstructionDescriptorTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineIRBuilderTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineIRCoreTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineVerifierTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineValueTypesTest.cpp"
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/CoreTypesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/InstructionDescriptorTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineIRBuilderTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineIRCoreTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineVerifierTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineValueTypesTest.cpp"
)
