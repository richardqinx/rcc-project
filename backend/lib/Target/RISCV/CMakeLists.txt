# SPDX-License-Identifier: GPL-3.0-or-later

set(RCCBackendRISCVDescription "${CMAKE_CURRENT_SOURCE_DIR}/RISCV.rccdesc")
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

add_custom_command(
  OUTPUT
    "${RCCBackendRISCVGeneratedRegisterHeader}"
    "${RCCBackendRISCVGeneratedInstructionHeader}"
    "${RCCBackendRISCVGeneratedTables}"
  COMMAND
    "$<TARGET_FILE:RCCBackendDescriptionGenerator>"
    --input "${RCCBackendRISCVDescription}"
    --register-header "${RCCBackendRISCVGeneratedRegisterHeader}"
    --instruction-header "${RCCBackendRISCVGeneratedInstructionHeader}"
    --tables "${RCCBackendRISCVGeneratedTables}"
  DEPENDS RCCBackendDescriptionGenerator "${RCCBackendRISCVDescription}"
  COMMENT "Generating RCC RV64GC target tables"
  VERBATIM
)

add_custom_target(
  RCCBackendRISCVGeneratedTablesTarget
  DEPENDS
    "${RCCBackendRISCVGeneratedRegisterHeader}"
    "${RCCBackendRISCVGeneratedInstructionHeader}"
    "${RCCBackendRISCVGeneratedTables}"
)

set_source_files_properties(
  "${RCCBackendRISCVGeneratedRegisterHeader}"
  "${RCCBackendRISCVGeneratedInstructionHeader}"
  "${RCCBackendRISCVGeneratedTables}"
  PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE
)

add_library(
  RCCBackendRISCV
  RISCVBranchInfo.cpp
  RISCVCodeGen.cpp
  RISCVCallLowering.cpp
  RISCVFrameLowering.cpp
  RISCVMachineCombiner.cpp
  RISCVMachineMemoryFolding.cpp
  RISCVOptimizationPipeline.cpp
  RISCVPseudoExpansion.cpp
  RISCVRegisterAllocation.cpp
  RISCVScheduling.cpp
  RISCVTarget.cpp
  "${RCCBackendRISCVGeneratedRegisterHeader}"
  "${RCCBackendRISCVGeneratedInstructionHeader}"
  "${RCCBackendRISCVGeneratedTables}"
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
  PRIVATE "${RCC_BACKEND_BINARY_DIR}/generated/lib/Target/RISCV"
)
target_link_libraries(RCCBackendRISCV PUBLIC RCCBackend::CodeGen)

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
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVBranchInfo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVCodeGen.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVCallLowering.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVFrameLowering.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMachineCombiner.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMachineMemoryFolding.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVOptimizationPipeline.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVPseudoExpansion.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVRegisterAllocation.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVScheduling.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVTarget.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVCodeGen.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVCallLowering.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVTarget.h"
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVBranchInfo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVCodeGen.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVCallLowering.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVFrameLowering.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMachineCombiner.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVMachineMemoryFolding.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVOptimizationPipeline.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVPseudoExpansion.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVRegisterAllocation.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVScheduling.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVTarget.cpp"
)
