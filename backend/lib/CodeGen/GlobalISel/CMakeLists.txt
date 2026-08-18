# SPDX-License-Identifier: GPL-3.0-or-later

set(
  RCCBackendGlobalISelSources
  CSEInfo.cpp
  CSEMIRBuilder.cpp
  Combiner.cpp
  GIMatchTableExecutor.cpp
  GISelChangeObserver.cpp
  GISelValueTracking.cpp
  InstructionSelect.cpp
  Legalizer.cpp
  LegalizerHelper.cpp
  LegalizerInfo.cpp
  RegBankSelect.cpp
  Utils.cpp
)

set(
  RCCBackendGlobalISelHeaders
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/CSEInfo.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/CSEMIRBuilder.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/Combiner.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/GIMatchTableExecutor.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/GISelChangeObserver.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/GISelValueTracking.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/GISelWorkList.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/GenericMachineInstrs.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/InstructionSelect.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/InstructionSelector.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/LegalizationArtifactCombiner.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/Legalizer.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/LegalizerHelper.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/LegalizerInfo.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/RegBankSelect.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/GlobalISel/Utils.h"
)

target_sources(RCCBackendCodeGen PRIVATE ${RCCBackendGlobalISelSources})

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/CSEInfo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/CSEMIRBuilder.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Combiner.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GIMatchTableExecutor.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GISelChangeObserver.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GISelValueTracking.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/InstructionSelect.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Legalizer.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LegalizerHelper.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LegalizerInfo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RegBankSelect.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Utils.cpp"
  ${RCCBackendGlobalISelHeaders}
)

rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/CSEInfo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/CSEMIRBuilder.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Combiner.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GIMatchTableExecutor.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GISelChangeObserver.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GISelValueTracking.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/InstructionSelect.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Legalizer.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LegalizerHelper.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LegalizerInfo.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/RegBankSelect.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Utils.cpp"
)
