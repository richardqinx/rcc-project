# SPDX-License-Identifier: GPL-3.0-or-later

set(
  RCCBackendRISCVDescriptionSources
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCV.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVRegisterInfo.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVCallingConv.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVInstrInfoBase.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVInstrInfoI.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVInstrInfoM.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVInstrInfoA.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVInstrInfoF.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVInstrInfoD.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVInstrInfoC.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVInstrInfoZb.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVInstrInfoPseudo.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVGISelInteger.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVGISelAddressing.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVGISelControlFlow.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVGISelShift.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVGISelFloating.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVGISelCombiner.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMacroFusion.rccdesc"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMCPseudoLowering.rccdesc"
)
set(RCCBackendRISCVTableGenArguments)
foreach(RCCBackendRISCVDescriptionSource IN LISTS RCCBackendRISCVDescriptionSources)
  list(APPEND RCCBackendRISCVTableGenArguments --input "${RCCBackendRISCVDescriptionSource}")
endforeach()
set(
  RCCBackendRISCVGeneratedRegisterHeader
  "${RCC_BACKEND_BINARY_DIR}/generated/include/rcc/Target/RISCV/RISCVRegisters.h"
)
set(
  RCCBackendRISCVGeneratedInstructionHeader
  "${RCC_BACKEND_BINARY_DIR}/generated/include/rcc/Target/RISCV/RISCVInstructions.h"
)
set(
  RCCBackendRISCVGeneratedTables
  "${RCC_BACKEND_BINARY_DIR}/generated/lib/Target/RISCV/RISCVGenTargetDescription.inc"
)
set(
  RCCBackendRISCVGeneratedGlobalISel
  "${RCC_BACKEND_BINARY_DIR}/generated/lib/Target/RISCV/RISCVGenGlobalISel.inc"
)
set(
  RCCBackendRISCVGeneratedGlobalISelCombiner
  "${RCC_BACKEND_BINARY_DIR}/generated/lib/Target/RISCV/RISCVGenGlobalISelCombiner.inc"
)
set(
  RCCBackendRISCVGeneratedMacroFusion
  "${RCC_BACKEND_BINARY_DIR}/generated/lib/Target/RISCV/RISCVGenMacroFusion.inc"
)
set(
  RCCBackendRISCVGeneratedMCPseudoLowering
  "${RCC_BACKEND_BINARY_DIR}/generated/lib/Target/RISCV/RISCVGenMCPseudoLowering.inc"
)

add_custom_command(
  OUTPUT
    "${RCCBackendRISCVGeneratedRegisterHeader}"
    "${RCCBackendRISCVGeneratedInstructionHeader}"
    "${RCCBackendRISCVGeneratedTables}"
    "${RCCBackendRISCVGeneratedGlobalISel}"
    "${RCCBackendRISCVGeneratedGlobalISelCombiner}"
    "${RCCBackendRISCVGeneratedMacroFusion}"
    "${RCCBackendRISCVGeneratedMCPseudoLowering}"
  COMMAND
    "$<TARGET_FILE:RCCBackendTableGenTool>"
    ${RCCBackendRISCVTableGenArguments}
    --register-header "${RCCBackendRISCVGeneratedRegisterHeader}"
    --instruction-header "${RCCBackendRISCVGeneratedInstructionHeader}"
    --tables "${RCCBackendRISCVGeneratedTables}"
    --global-isel "${RCCBackendRISCVGeneratedGlobalISel}"
    --global-isel-combiner "${RCCBackendRISCVGeneratedGlobalISelCombiner}"
    --macro-fusion "${RCCBackendRISCVGeneratedMacroFusion}"
    --mc-pseudo-lowering "${RCCBackendRISCVGeneratedMCPseudoLowering}"
  DEPENDS RCCBackendTableGenTool ${RCCBackendRISCVDescriptionSources}
  COMMENT "Generating RCC RV64GC target tables"
  VERBATIM
)

add_custom_target(
  RCCBackendRISCVGeneratedTablesTarget
  DEPENDS
    "${RCCBackendRISCVGeneratedRegisterHeader}"
    "${RCCBackendRISCVGeneratedInstructionHeader}"
    "${RCCBackendRISCVGeneratedTables}"
    "${RCCBackendRISCVGeneratedGlobalISel}"
    "${RCCBackendRISCVGeneratedGlobalISelCombiner}"
    "${RCCBackendRISCVGeneratedMacroFusion}"
    "${RCCBackendRISCVGeneratedMCPseudoLowering}"
)

set_source_files_properties(
  "${RCCBackendRISCVGeneratedRegisterHeader}"
  "${RCCBackendRISCVGeneratedInstructionHeader}"
  "${RCCBackendRISCVGeneratedTables}"
  "${RCCBackendRISCVGeneratedGlobalISel}"
  "${RCCBackendRISCVGeneratedGlobalISelCombiner}"
  "${RCCBackendRISCVGeneratedMacroFusion}"
  "${RCCBackendRISCVGeneratedMCPseudoLowering}"
  PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE
)

add_library(
  RCCBackendRISCV
  GISel/RISCVCallLowering.cpp
  GISel/RISCVInstructionSelector.cpp
  GISel/RISCVGISelCombiner.cpp
  GISel/RISCVLegalizerInfo.cpp
  GISel/RISCVRegisterBankInfo.cpp
  RISCVInstrInfo.cpp
  RISCVAssemblyPrinter.cpp
  RISCVFrameLowering.cpp
  RISCVFunctionInfo.cpp
  RISCVImmediateMaterialization.cpp
  RISCVInstPrinter.cpp
  RISCVAsmBackend.cpp
  RISCVMCCodeEmitter.cpp
  RISCVObjectFile.cpp
  RISCVMCTargetLowering.cpp
  RISCVMachineCombiner.cpp
  RISCVMachineMemoryFolding.cpp
  RISCVOptimizationPipeline.cpp
  RISCVPipeline.cpp
  RISCVPseudoExpansion.cpp
  RISCVRegisterAllocation.cpp
  RISCVScheduling.cpp
  RISCVSubtarget.cpp
  RISCVTarget.cpp
  RISCVTargetConstant.cpp
  "${RCCBackendRISCVGeneratedRegisterHeader}"
  "${RCCBackendRISCVGeneratedInstructionHeader}"
  "${RCCBackendRISCVGeneratedTables}"
  "${RCCBackendRISCVGeneratedGlobalISel}"
  "${RCCBackendRISCVGeneratedGlobalISelCombiner}"
  "${RCCBackendRISCVGeneratedMacroFusion}"
  "${RCCBackendRISCVGeneratedMCPseudoLowering}"
)
add_library(RCCBackend::RISCV ALIAS RCCBackendRISCV)
add_dependencies(RCCBackendRISCV RCCBackendRISCVGeneratedTablesTarget)

rcc_backend_configure_target(RCCBackendRISCV)

target_include_directories(
  RCCBackendRISCV
  PUBLIC
    "$<BUILD_INTERFACE:${RCC_BACKEND_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${RCC_BACKEND_BINARY_DIR}/generated/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${RCC_BACKEND_BINARY_DIR}/generated/lib/Target/RISCV"
)
target_link_libraries(
  RCCBackendRISCV
  PUBLIC
    RCCBackend::AsmPrinter
    RCCBackend::CodeGen
    RCCBackend::MC
)

set_target_properties(
  RCCBackendRISCV
  PROPERTIES
    EXPORT_NAME RISCV
    OUTPUT_NAME RCCBackendRISCV
)

install(
  TARGETS RCCBackendRISCV
  EXPORT RCCBackendTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
  FILES
    "${RCCBackendRISCVGeneratedRegisterHeader}"
    "${RCCBackendRISCVGeneratedInstructionHeader}"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/rcc/Target/RISCV"
)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/GISel/RISCVCallLowering.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GISel/RISCVCallLowering.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/GISel/RISCVInstructionSelector.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GISel/RISCVGISelCombiner.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GISel/RISCVLegalizerInfo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GISel/RISCVLegalizerInfo.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/GISel/RISCVRegisterBankInfo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GISel/RISCVRegisterBankInfo.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCV.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVInstrInfo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVAssemblyPrinter.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVFrameLowering.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVFunctionInfo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVImmediateMaterialization.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVImmediateMaterialization.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVInstPrinter.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVAsmBackend.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMCCodeEmitter.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVObjectFile.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMCTargetLowering.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMachineCombiner.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMachineMemoryFolding.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVOptimizationPipeline.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVPipeline.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVPseudoExpansion.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVRegisterAllocation.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVScheduling.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVSubtarget.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVTarget.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVTargetConstant.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVInstrInfo.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVAssemblyPrinter.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVInstPrinter.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVAsmBackend.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVFixupKinds.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVFunctionInfo.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVMCCodeEmitter.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVObjectFile.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVPipeline.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVSubtarget.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVMCTargetLowering.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVTarget.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVTargetConstant.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVTargetRelocationKinds.h"
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/GISel/RISCVCallLowering.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GISel/RISCVInstructionSelector.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GISel/RISCVGISelCombiner.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GISel/RISCVLegalizerInfo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GISel/RISCVRegisterBankInfo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVInstrInfo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVAssemblyPrinter.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVFrameLowering.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVFunctionInfo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVImmediateMaterialization.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVInstPrinter.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVAsmBackend.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMCCodeEmitter.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVObjectFile.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMCTargetLowering.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMachineCombiner.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMachineMemoryFolding.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVOptimizationPipeline.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVPipeline.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVPseudoExpansion.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVRegisterAllocation.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVScheduling.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVSubtarget.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVTarget.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVTargetConstant.cpp"
)
