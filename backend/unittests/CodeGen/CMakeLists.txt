# SPDX-License-Identifier: GPL-3.0-or-later

set(
  RCCBackendCodeGenTestSources
  CompilationUnitTest.cpp
  CoreTypesTest.cpp
  GenericOpcodesTest.cpp
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
  MachineAnalysesTest.cpp
  MachineJumpTableInfoTest.cpp
  SlotIndexesTest.cpp
  LiveVariablesTest.cpp
  LiveIntervalTest.cpp
  LiveIntervalsTest.cpp
  LivePhysRegsTest.cpp
  LiveRegUnitsTest.cpp
  LiveTestSupport.h
  MachineMetadataTest.cpp
  MachineOperandTest.cpp
  MachineOptimizationTest.cpp
  PassManagerTest.cpp
  PassInstrumentationTest.cpp
  MachinePassManagerTest.cpp
  MachineSchedulingTest.cpp
  PBQPTest.cpp
  RegisterAllocationTest.cpp
  PHIEliminationTest.cpp
  TwoAddressInstructionPassTest.cpp
  MachineVerifierTest.cpp
  MachineVerifierTargetTest.cpp
  MachineValueTypesTest.cpp
  TargetConstantTest.cpp
  TargetCallLoweringTest.cpp
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

add_executable(RCCBackendCodeGenPBQPTests PBQPTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenPBQPTests)
target_link_libraries(RCCBackendCodeGenPBQPTests PRIVATE RCCBackend::CodeGen)

add_test(NAME RCCBackend.CodeGen.PBQP COMMAND RCCBackendCodeGenPBQPTests)

add_executable(RCCBackendCodeGenRegisterAllocationTests RegisterAllocationTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenRegisterAllocationTests)
target_link_libraries(
  RCCBackendCodeGenRegisterAllocationTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)

add_test(
  NAME RCCBackend.CodeGen.RegisterAllocation
  COMMAND RCCBackendCodeGenRegisterAllocationTests
)

add_executable(RCCBackendCodeGenLiveRegMatrixTests LiveRegMatrixTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenLiveRegMatrixTests)
target_link_libraries(
  RCCBackendCodeGenLiveRegMatrixTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)
add_test(NAME RCCBackend.CodeGen.LiveRegMatrix COMMAND RCCBackendCodeGenLiveRegMatrixTests)

add_executable(RCCBackendCodeGenVirtRegMapTests VirtRegMapTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenVirtRegMapTests)
target_link_libraries(
  RCCBackendCodeGenVirtRegMapTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)
add_test(NAME RCCBackend.CodeGen.VirtRegMap COMMAND RCCBackendCodeGenVirtRegMapTests)

add_executable(RCCBackendCodeGenSpillerTests SpillerTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenSpillerTests)
target_link_libraries(
  RCCBackendCodeGenSpillerTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)
add_test(NAME RCCBackend.CodeGen.Spiller COMMAND RCCBackendCodeGenSpillerTests)

add_executable(RCCBackendCodeGenBasicAllocatorTests RegAllocBasicTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenBasicAllocatorTests)
target_link_libraries(
  RCCBackendCodeGenBasicAllocatorTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)
add_test(NAME RCCBackend.CodeGen.BasicAllocator COMMAND RCCBackendCodeGenBasicAllocatorTests)

add_executable(RCCBackendCodeGenFastAllocatorTests RegAllocFastTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenFastAllocatorTests)
target_link_libraries(
  RCCBackendCodeGenFastAllocatorTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)
add_test(NAME RCCBackend.CodeGen.FastAllocator COMMAND RCCBackendCodeGenFastAllocatorTests)

add_executable(RCCBackendCodeGenPBQPAllocatorTests RegAllocPBQPTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenPBQPAllocatorTests)
target_link_libraries(
  RCCBackendCodeGenPBQPAllocatorTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)
add_test(NAME RCCBackend.CodeGen.PBQPAllocator COMMAND RCCBackendCodeGenPBQPAllocatorTests)

add_executable(RCCBackendCodeGenGenericOpcodesTests GenericOpcodesTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenGenericOpcodesTests)
target_link_libraries(RCCBackendCodeGenGenericOpcodesTests PRIVATE RCCBackend::CodeGen)

add_test(NAME RCCBackend.CodeGen.GenericOpcodes COMMAND RCCBackendCodeGenGenericOpcodesTests)

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

foreach(
  Component
  SlotIndexes
  LiveVariables
  LiveInterval
  LiveIntervals
  LivePhysRegs
  LiveRegUnits
)
  add_executable(RCCBackendCodeGen${Component}Tests ${Component}Test.cpp)
  rcc_backend_configure_target(RCCBackendCodeGen${Component}Tests)
  target_link_libraries(
    RCCBackendCodeGen${Component}Tests
    PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget RCCBackend::RISCV
  )
  add_test(
    NAME RCCBackend.CodeGen.${Component}
    COMMAND RCCBackendCodeGen${Component}Tests
  )
endforeach()

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

add_executable(RCCBackendCodeGenPassManagerTests PassManagerTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenPassManagerTests)
target_link_libraries(
  RCCBackendCodeGenPassManagerTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)
add_test(NAME RCCBackend.CodeGen.PassManager COMMAND RCCBackendCodeGenPassManagerTests)

add_executable(RCCBackendCodeGenPassInstrumentationTests PassInstrumentationTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenPassInstrumentationTests)
target_link_libraries(
  RCCBackendCodeGenPassInstrumentationTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)
add_test(
  NAME RCCBackend.CodeGen.PassInstrumentation
  COMMAND RCCBackendCodeGenPassInstrumentationTests
)

add_executable(RCCBackendCodeGenMachinePassManagerTests MachinePassManagerTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenMachinePassManagerTests)
target_link_libraries(
  RCCBackendCodeGenMachinePassManagerTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)
add_test(
  NAME RCCBackend.CodeGen.MachinePassManager
  COMMAND RCCBackendCodeGenMachinePassManagerTests
)

add_subdirectory(GlobalISel)

add_executable(RCCBackendCodeGenMachineAnalysesTests MachineAnalysesTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenMachineAnalysesTests)
target_link_libraries(
  RCCBackendCodeGenMachineAnalysesTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)

add_test(NAME RCCBackend.CodeGen.MachineAnalyses COMMAND RCCBackendCodeGenMachineAnalysesTests)

add_executable(RCCBackendCodeGenMachineOptimizationTests MachineOptimizationTest.cpp)
target_include_directories(
  RCCBackendCodeGenMachineOptimizationTests
  PRIVATE "${RCC_BACKEND_SOURCE_DIR}/lib/Target/RISCV"
)
rcc_backend_configure_target(RCCBackendCodeGenMachineOptimizationTests)
target_link_libraries(
  RCCBackendCodeGenMachineOptimizationTests
  PRIVATE RCCBackend::CodeGen RCCBackend::RISCV
)
add_test(NAME RCCBackend.CodeGen.MachineOptimization COMMAND RCCBackendCodeGenMachineOptimizationTests)

add_executable(RCCBackendCodeGenMachineSchedulingTests MachineSchedulingTest.cpp)
target_include_directories(
  RCCBackendCodeGenMachineSchedulingTests
  PRIVATE "${RCC_BACKEND_SOURCE_DIR}/lib/Target/RISCV"
)
rcc_backend_configure_target(RCCBackendCodeGenMachineSchedulingTests)
target_link_libraries(
  RCCBackendCodeGenMachineSchedulingTests
  PRIVATE RCCBackend::CodeGen RCCBackend::RISCV
)
add_test(NAME RCCBackend.CodeGen.MachineScheduling COMMAND RCCBackendCodeGenMachineSchedulingTests)

add_executable(RCCBackendCodeGenPHIEliminationTests PHIEliminationTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenPHIEliminationTests)
target_link_libraries(
  RCCBackendCodeGenPHIEliminationTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)
add_test(
  NAME RCCBackend.CodeGen.PHIElimination
  COMMAND RCCBackendCodeGenPHIEliminationTests
)

add_executable(
  RCCBackendCodeGenTwoAddressInstructionPassTests
  TwoAddressInstructionPassTest.cpp
)
rcc_backend_configure_target(RCCBackendCodeGenTwoAddressInstructionPassTests)
target_link_libraries(
  RCCBackendCodeGenTwoAddressInstructionPassTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)
add_test(
  NAME RCCBackend.CodeGen.TwoAddressInstructionPass
  COMMAND RCCBackendCodeGenTwoAddressInstructionPassTests
)

add_executable(RCCBackendCodeGenTargetConstantTests TargetConstantTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenTargetConstantTests)
target_link_libraries(RCCBackendCodeGenTargetConstantTests PRIVATE RCCBackend::CodeGen)

add_test(
  NAME RCCBackend.CodeGen.TargetConstant
  COMMAND RCCBackendCodeGenTargetConstantTests
)

add_executable(RCCBackendCodeGenTargetCallLoweringTests TargetCallLoweringTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenTargetCallLoweringTests)
target_link_libraries(
  RCCBackendCodeGenTargetCallLoweringTests
  PRIVATE RCCBackend::CodeGen RCCBackend::TestTarget
)

add_test(
  NAME RCCBackend.CodeGen.TargetCallLowering
  COMMAND RCCBackendCodeGenTargetCallLoweringTests
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
  "${CMAKE_CURRENT_SOURCE_DIR}/GenericOpcodesTest.cpp"
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
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineAnalysesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineJumpTableInfoTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/SlotIndexesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LiveVariablesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LiveIntervalTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LiveIntervalsTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LivePhysRegsTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LiveRegUnitsTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LiveTestSupport.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineMetadataTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineOperandTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineOptimizationTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/PassManagerTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/PassInstrumentationTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachinePassManagerTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineSchedulingTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/PBQPTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RegisterAllocationTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/PHIEliminationTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/SSAExitTestSupport.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/TwoAddressInstructionPassTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineVerifierTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineVerifierTargetTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineValueTypesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetConstantTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetCallLoweringTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetFunctionStateTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetMIRTextInfoTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetRegisterInfoTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RegisterAllocationTestSupport.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/LiveRegMatrixTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/VirtRegMapTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/SpillerTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RegAllocBasicTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RegAllocFastTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RegAllocPBQPTest.cpp"
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/CompilationUnitTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/CoreTypesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GenericOpcodesTest.cpp"
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
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineAnalysesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineJumpTableInfoTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/SlotIndexesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LiveVariablesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LiveIntervalTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LiveIntervalsTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LivePhysRegsTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LiveRegUnitsTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LiveTestSupport.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineMetadataTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineOperandTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineOptimizationTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/PassManagerTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/PassInstrumentationTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachinePassManagerTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineSchedulingTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/PBQPTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RegisterAllocationTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/PHIEliminationTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/SSAExitTestSupport.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/TwoAddressInstructionPassTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineVerifierTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineVerifierTargetTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineValueTypesTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetConstantTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetCallLoweringTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetFunctionStateTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetMIRTextInfoTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetRegisterInfoTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LiveRegMatrixTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/VirtRegMapTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/SpillerTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RegAllocBasicTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RegAllocFastTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RegAllocPBQPTest.cpp"
)
