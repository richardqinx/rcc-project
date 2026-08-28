# SPDX-License-Identifier: GPL-3.0-or-later

set(RCCBackendAsmParserSources LLParser.cpp)
set(
  RCCBackendAsmParserHeaders
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/AsmParser/LLParser.h"
)

add_library(RCCBackendAsmParser ${RCCBackendAsmParserSources})
add_library(RCCBackend::AsmParser ALIAS RCCBackendAsmParser)

rcc_backend_configure_target(RCCBackendAsmParser)

target_include_directories(
  RCCBackendAsmParser
  PUBLIC
    "$<BUILD_INTERFACE:${RCC_BACKEND_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(RCCBackendAsmParser PUBLIC RCCBackend::IR)

set_target_properties(
  RCCBackendAsmParser
  PROPERTIES
    EXPORT_NAME AsmParser
    OUTPUT_NAME RCCBackendAsmParser
)

install(
  TARGETS RCCBackendAsmParser
  EXPORT RCCBackendTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/LLParser.cpp"
  ${RCCBackendAsmParserHeaders}
)
