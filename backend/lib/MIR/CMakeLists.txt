# SPDX-License-Identifier: GPL-3.0-or-later

set(
  RCCBackendMIRSources
  EmbeddedIR.cpp
  ReachingDefPrinter.cpp
  "${RCC_BACKEND_SOURCE_DIR}/lib/CodeGen/MIRParser/MILexer.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/lib/CodeGen/MIRParser/MIParser.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/lib/CodeGen/MIRParser/MIRParser.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/lib/CodeGen/MIRPrinter.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/lib/CodeGen/MIRYamlMapping.cpp"
)
set(
  RCCBackendMIRHeaders
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/MIRParser/MIParser.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/MIRParser/MIRParser.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/MIRPrinter.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/MIRYamlMapping.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MIR/EmbeddedIR.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MIR/ReachingDefPrinter.h"
)

add_library(RCCBackendMIR ${RCCBackendMIRSources})
add_library(RCCBackend::MIR ALIAS RCCBackendMIR)

rcc_backend_configure_target(RCCBackendMIR)

target_include_directories(
  RCCBackendMIR
  PUBLIC
    "$<BUILD_INTERFACE:${RCC_BACKEND_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(
  RCCBackendMIR
  PUBLIC
    RCCBackend::AsmParser
    RCCBackend::CodeGen
    RCCBackend::IR
)

set_target_properties(
  RCCBackendMIR
  PROPERTIES
    EXPORT_NAME MIR
    OUTPUT_NAME RCCBackendMIR
)

install(
  TARGETS RCCBackendMIR
  EXPORT RCCBackendTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/EmbeddedIR.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ReachingDefPrinter.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/lib/CodeGen/MIRParser/MILexer.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/lib/CodeGen/MIRParser/MILexer.h"
  "${RCC_BACKEND_SOURCE_DIR}/lib/CodeGen/MIRParser/MIParser.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/lib/CodeGen/MIRParser/MIRParser.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/lib/CodeGen/MIRPrinter.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/lib/CodeGen/MIRYamlMapping.cpp"
  ${RCCBackendMIRHeaders}
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/EmbeddedIR.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ReachingDefPrinter.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/lib/CodeGen/MIRParser/MILexer.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/lib/CodeGen/MIRParser/MIParser.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/lib/CodeGen/MIRParser/MIRParser.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/lib/CodeGen/MIRPrinter.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/lib/CodeGen/MIRYamlMapping.cpp"
)
