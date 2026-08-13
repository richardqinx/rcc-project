# SPDX-License-Identifier: GPL-3.0-or-later

set(
  RCCBackendTableGenBackendSources
  AsmWriterEmitter.cpp
  CallingConvEmitter.cpp
  CodeEmitterGen.cpp
  Common/CodeGenInstruction.cpp
  Common/CodeGenRegisters.cpp
  Common/CodeGenSchedule.cpp
  Common/CodeGenTarget.cpp
  InstrInfoEmitter.cpp
  RegisterBankEmitter.cpp
  RegisterInfoEmitter.cpp
  SearchableTableEmitter.cpp
  SubtargetEmitter.cpp
)
list(TRANSFORM RCCBackendTableGenBackendSources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
set(
  RCCBackendTableGenBackendHeaders
  "${CMAKE_CURRENT_SOURCE_DIR}/Common/CodeGenTarget.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/TableGenBackends.h"
)

add_library(RCCBackendTableGenBackends STATIC ${RCCBackendTableGenBackendSources})
add_library(RCCBackend::TableGenBackends ALIAS RCCBackendTableGenBackends)
rcc_backend_configure_target(RCCBackendTableGenBackends)
target_include_directories(
  RCCBackendTableGenBackends
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)
target_link_libraries(RCCBackendTableGenBackends PUBLIC RCCBackend::TableGen)

add_executable(RCCBackendTableGenTool rcc-tblgen.cpp)
rcc_backend_configure_target(RCCBackendTableGenTool)
target_link_libraries(RCCBackendTableGenTool PRIVATE RCCBackend::TableGenBackends)
set_target_properties(RCCBackendTableGenTool PROPERTIES OUTPUT_NAME rcc-tblgen)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/rcc-tblgen.cpp"
  ${RCCBackendTableGenBackendSources}
  ${RCCBackendTableGenBackendHeaders}
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/rcc-tblgen.cpp"
  ${RCCBackendTableGenBackendSources}
)
