# SPDX-License-Identifier: GPL-3.0-or-later

include_directories("${RCC_BACKEND_SOURCE_DIR}/lib/Target/RISCV")

add_executable(RCCBackendCodeGenLegalizerTests LegalizerTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenLegalizerTests)
target_link_libraries(
  RCCBackendCodeGenLegalizerTests
  PRIVATE RCCBackend::CodeGen RCCBackend::RISCV RCCBackend::TestTarget
)
add_test(NAME RCCBackend.CodeGen.GlobalISel.Legalizer COMMAND RCCBackendCodeGenLegalizerTests)

add_executable(RCCBackendCodeGenRegBankSelectTests RegBankSelectTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenRegBankSelectTests)
target_link_libraries(
  RCCBackendCodeGenRegBankSelectTests
  PRIVATE RCCBackend::CodeGen RCCBackend::RISCV RCCBackend::TestTarget
)
add_test(
  NAME RCCBackend.CodeGen.GlobalISel.RegBankSelect
  COMMAND RCCBackendCodeGenRegBankSelectTests
)

add_executable(RCCBackendCodeGenInstructionSelectTests InstructionSelectTest.cpp)
rcc_backend_configure_target(RCCBackendCodeGenInstructionSelectTests)
target_link_libraries(
  RCCBackendCodeGenInstructionSelectTests
  PRIVATE RCCBackend::CodeGen RCCBackend::RISCV RCCBackend::TestTarget
)
add_test(
  NAME RCCBackend.CodeGen.GlobalISel.InstructionSelect
  COMMAND RCCBackendCodeGenInstructionSelectTests
)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/GISelMITest.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/InstructionSelectTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LegalizerTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RegBankSelectTest.cpp"
)
