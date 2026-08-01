# SPDX-License-Identifier: GPL-3.0-or-later

set(
  RCCBackendCodeGenSources
  InstructionDescriptor.cpp
  LowLevelType.cpp
  MachineBasicBlock.cpp
  MachineCFGUpdater.cpp
  MachineFunction.cpp
  MachineIRTypes.cpp
  MachineIRBuilder.cpp
  MachineInstr.cpp
  MachineMemOperand.cpp
  MachineOperand.cpp
  MachineRegisterInfo.cpp
  MachineVerifier.cpp
  MachineFunctionProperties.cpp
  Register.cpp
  RegisterMask.cpp
)
set(
  RCCBackendCodeGenHeaders
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/InstructionDescriptor.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/LowLevelType.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/MachineBasicBlock.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/MachineCFGUpdater.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/MachineFunction.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/MachineIRTypes.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/MachineIRBuilder.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/MachineInstr.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/MachineMemOperand.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/MachineOperand.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/MachineRegisterInfo.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/MachineVerifier.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/MachineFunctionProperties.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/Register.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/RegisterMask.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/StableID.h"
)

add_library(RCCBackendCodeGen ${RCCBackendCodeGenSources})
add_library(RCCBackend::CodeGen ALIAS RCCBackendCodeGen)

rcc_backend_configure_target(RCCBackendCodeGen)

target_include_directories(
  RCCBackendCodeGen
  PUBLIC
    "$<BUILD_INTERFACE:${RCC_BACKEND_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(RCCBackendCodeGen PUBLIC RCCBackend::Support)

set_target_properties(
  RCCBackendCodeGen
  PROPERTIES
    EXPORT_NAME CodeGen
    OUTPUT_NAME RCCBackendCodeGen
)

install(
  TARGETS RCCBackendCodeGen
  EXPORT RCCBackendTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/InstructionDescriptor.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LowLevelType.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineBasicBlock.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineCFGUpdater.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineFunction.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineIRTypes.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineIRBuilder.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineInstr.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineMemOperand.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineOperand.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineRegisterInfo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineVerifier.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineFunctionProperties.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Register.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RegisterMask.cpp"
  ${RCCBackendCodeGenHeaders}
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/InstructionDescriptor.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LowLevelType.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineBasicBlock.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineCFGUpdater.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineFunction.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineIRTypes.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineIRBuilder.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineInstr.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineMemOperand.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineOperand.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineRegisterInfo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineVerifier.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MachineFunctionProperties.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Register.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RegisterMask.cpp"
)
