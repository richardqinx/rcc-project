# SPDX-License-Identifier: GPL-3.0-or-later

set(
  RCCBackendGlobalISelSources
  InstructionSelect.cpp
  Legalizer.cpp
  RegBankSelect.cpp
)

set(
  RCCBackendGlobalISelHeaders
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/InstructionSelect.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/InstructionSelector.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/Legalizer.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/LegalizerInfo.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/RegBankSelect.h"
)

target_sources(RCCBackendCodeGen PRIVATE ${RCCBackendGlobalISelSources})

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/InstructionSelect.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Legalizer.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RegBankSelect.cpp"
  ${RCCBackendGlobalISelHeaders}
)

rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/InstructionSelect.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Legalizer.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RegBankSelect.cpp"
)
