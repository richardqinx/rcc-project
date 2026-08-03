# SPDX-License-Identifier: GPL-3.0-or-later

set(
  RCCBackendCodeGenTestSources
  CompilationUnitTest.cpp
  CoreTypesTest.cpp
  InstructionDescriptorTest.cpp
  MachineCompilationStatusTest.cpp
  MachineConstantPoolTest.cpp
  MachineDebugInfoTest.cpp
  MachineFrameInfoTest.cpp
  MachineFunctionClonerTest.cpp
  MachineFunctionRecordsTest.cpp
  MachineIRAttributesTest.cpp
  MachineIRBuilderTest.cpp
  MachineIRCoreTest.cpp
  MachineJumpTableInfoTest.cpp
  MachineMetadataTest.cpp
  MachineOperandTest.cpp
  MachineVerifierTest.cpp
  MachineVerifierTargetTest.cpp
  MachineValueTypesTest.cpp
  TargetConstantTest.cpp
  TargetFunctionStateTest.cpp
  TargetMIRTextInfoTest.cpp
  TargetRegisterInfoTest.cpp
)

add_executable(RCCBackendCodeGenCompilationUnitTests CompilationUnitTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenCompilationUnitTests)
target_link_libraries(RCCBackendCodeGenCompilationUnitTests PRIVATE RCCBackend::CodeGen)

add_test(
  NAME RCCBackend.CodeGen.CompilationUnit
  COMMAND RCCBackendCodeGenCompilationUnitTests
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

add_executable(
  RCCBackendCodeGenMachineCompilationStatusTests
  MachineCompilationStatusTest.cpp
)
rcc_backend_configure_target(RCCBackendCodeGenMachineCompilationStatusTests)
target_link_libraries(
  RCCBackendCodeGenMachineCompilationStatusTests
  PRIVATE RCCBackend::CodeGen
)

add_test(
  NAME RCCBackend.CodeGen.MachineCompilationStatus
  COMMAND RCCBackendCodeGenMachineCompilationStatusTests
)

add_executable(RCCBackendCodeGenMachineConstantPoolTests MachineConstantPoolTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenMachineConstantPoolTests)
target_link_libraries(RCCBackendCodeGenMachineConstantPoolTests PRIVATE RCCBackend::CodeGen)

add_test(
  NAME RCCBackend.CodeGen.MachineConstantPool
  COMMAND RCCBackendCodeGenMachineConstantPoolTests
)

add_executable(RCCBackendCodeGenMachineDebugInfoTests MachineDebugInfoTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenMachineDebugInfoTests)
target_link_libraries(
  RCCBackendCodeGenMachineDebugInfoTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)

add_test(
  NAME RCCBackend.CodeGen.MachineDebugInfo
  COMMAND RCCBackendCodeGenMachineDebugInfoTests
)

add_executable(RCCBackendCodeGenMachineFrameInfoTests MachineFrameInfoTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenMachineFrameInfoTests)
target_link_libraries(RCCBackendCodeGenMachineFrameInfoTests PRIVATE RCCBackend::CodeGen)

add_test(
  NAME RCCBackend.CodeGen.MachineFrameInfo
  COMMAND RCCBackendCodeGenMachineFrameInfoTests
)

add_executable(
  RCCBackendCodeGenMachineFunctionClonerTests
  MachineFunctionClonerTest.cpp
)
rcc_backend_configure_target(RCCBackendCodeGenMachineFunctionClonerTests)
target_link_libraries(
  RCCBackendCodeGenMachineFunctionClonerTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)

add_test(
  NAME RCCBackend.CodeGen.MachineFunctionCloner
  COMMAND RCCBackendCodeGenMachineFunctionClonerTests
)

add_executable(
  RCCBackendCodeGenMachineFunctionRecordsTests
  MachineFunctionRecordsTest.cpp
)
rcc_backend_configure_target(RCCBackendCodeGenMachineFunctionRecordsTests)
target_link_libraries(
  RCCBackendCodeGenMachineFunctionRecordsTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)

add_test(
  NAME RCCBackend.CodeGen.MachineFunctionRecords
  COMMAND RCCBackendCodeGenMachineFunctionRecordsTests
)

add_executable(RCCBackendCodeGenMachineIRBuilderTests MachineIRBuilderTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenMachineIRBuilderTests)
target_link_libraries(
  RCCBackendCodeGenMachineIRBuilderTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)

add_test(
  NAME RCCBackend.CodeGen.MachineIRBuilder
  COMMAND RCCBackendCodeGenMachineIRBuilderTests
)

add_executable(RCCBackendCodeGenMachineIRAttributesTests MachineIRAttributesTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenMachineIRAttributesTests)
target_link_libraries(
  RCCBackendCodeGenMachineIRAttributesTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)

add_test(
  NAME RCCBackend.CodeGen.MachineIRAttributes
  COMMAND RCCBackendCodeGenMachineIRAttributesTests
)

add_executable(RCCBackendCodeGenMachineVerifierTests MachineVerifierTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenMachineVerifierTests)
target_link_libraries(
  RCCBackendCodeGenMachineVerifierTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)

add_test(
  NAME RCCBackend.CodeGen.MachineVerifier
  COMMAND RCCBackendCodeGenMachineVerifierTests
)

add_executable(
  RCCBackendCodeGenMachineVerifierTargetTests
  MachineVerifierTargetTest.cpp
)
rcc_backend_configure_target(RCCBackendCodeGenMachineVerifierTargetTests)
target_link_libraries(
  RCCBackendCodeGenMachineVerifierTargetTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)

add_test(
  NAME RCCBackend.CodeGen.MachineVerifierTarget
  COMMAND RCCBackendCodeGenMachineVerifierTargetTests
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

add_executable(RCCBackendCodeGenMachineJumpTableInfoTests MachineJumpTableInfoTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenMachineJumpTableInfoTests)
target_link_libraries(RCCBackendCodeGenMachineJumpTableInfoTests PRIVATE RCCBackend::CodeGen)

add_test(
  NAME RCCBackend.CodeGen.MachineJumpTableInfo
  COMMAND RCCBackendCodeGenMachineJumpTableInfoTests
)

add_executable(RCCBackendCodeGenMachineMetadataTests MachineMetadataTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenMachineMetadataTests)
target_link_libraries(
  RCCBackendCodeGenMachineMetadataTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)

add_test(
  NAME RCCBackend.CodeGen.MachineMetadata
  COMMAND RCCBackendCodeGenMachineMetadataTests
)

add_executable(RCCBackendCodeGenMachineOperandTests MachineOperandTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenMachineOperandTests)
target_link_libraries(
  RCCBackendCodeGenMachineOperandTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)

add_test(
  NAME RCCBackend.CodeGen.MachineOperand
  COMMAND RCCBackendCodeGenMachineOperandTests
)

add_executable(RCCBackendCodeGenTargetConstantTests TargetConstantTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenTargetConstantTests)
target_link_libraries(RCCBackendCodeGenTargetConstantTests PRIVATE RCCBackend::CodeGen)

add_test(
  NAME RCCBackend.CodeGen.TargetConstant
  COMMAND RCCBackendCodeGenTargetConstantTests
)

add_executable(RCCBackendCodeGenTargetFunctionStateTests TargetFunctionStateTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenTargetFunctionStateTests)
target_link_libraries(
  RCCBackendCodeGenTargetFunctionStateTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)

add_test(
  NAME RCCBackend.CodeGen.TargetFunctionState
  COMMAND RCCBackendCodeGenTargetFunctionStateTests
)

add_executable(RCCBackendCodeGenTargetMIRTextInfoTests TargetMIRTextInfoTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenTargetMIRTextInfoTests)
target_link_libraries(
  RCCBackendCodeGenTargetMIRTextInfoTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)

add_test(
  NAME RCCBackend.CodeGen.TargetMIRTextInfo
  COMMAND RCCBackendCodeGenTargetMIRTextInfoTests
)

add_executable(RCCBackendCodeGenTargetRegisterInfoTests TargetRegisterInfoTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenTargetRegisterInfoTests)
target_link_libraries(
  RCCBackendCodeGenTargetRegisterInfoTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)

add_test(
  NAME RCCBackend.CodeGen.TargetRegisterInfo
  COMMAND RCCBackendCodeGenTargetRegisterInfoTests
)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/CompilationUnitTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/CoreTypesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/InstructionDescriptorTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineCompilationStatusTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineConstantPoolTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineDebugInfoTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineFrameInfoTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineFunctionClonerTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineFunctionRecordsTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineIRAttributesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineIRBuilderTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineIRCoreTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineJumpTableInfoTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineMetadataTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineOperandTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineVerifierTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineVerifierTargetTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineValueTypesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetConstantTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetFunctionStateTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetMIRTextInfoTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetRegisterInfoTest.cpp"
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/CompilationUnitTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/CoreTypesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/InstructionDescriptorTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineCompilationStatusTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineConstantPoolTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineDebugInfoTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineFrameInfoTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineFunctionClonerTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineFunctionRecordsTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineIRAttributesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineIRBuilderTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineIRCoreTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineJumpTableInfoTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineMetadataTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineOperandTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineVerifierTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineVerifierTargetTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineValueTypesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetConstantTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetFunctionStateTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetMIRTextInfoTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetRegisterInfoTest.cpp"
)
