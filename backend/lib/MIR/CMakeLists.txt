# SPDX-License-Identifier: GPL-3.0-or-later

set(
  RCCBackendMIRSources
  EmbeddedIR.cpp
  LLVMConstantSyntax.cpp
  MIDebugSyntax.cpp
  MILexer.cpp
  MIRDataParser.cpp
  MIRDocument.cpp
  MIRDocumentParser.cpp
  MIRDocumentReferenceValidation.cpp
  MIRDocumentPrinter.cpp
  MIRFrameParser.cpp
  MIRFunctionParser.cpp
  MIRFunctionRecordsParser.cpp
  MIRMetadataParser.cpp
  MIRMetadataTextParser.cpp
  MIParser.cpp
  MIPrinter.cpp
)
set(
  RCCBackendMIRHeaders
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MIR/EmbeddedIR.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MIR/LLVMConstantSyntax.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MIR/MIDebugSyntax.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MIR/MILexer.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MIR/MIRDocument.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MIR/MIParser.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MIR/MIPrinter.h"
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
target_link_libraries(RCCBackendMIR PUBLIC RCCBackend::CodeGen)

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
  "${CMAKE_CURRENT_SOURCE_DIR}/LLVMConstantSyntax.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIDebugSyntax.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MILexer.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRDataParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRDocument.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRDocumentParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRDocumentParser.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRDocumentReferenceValidation.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRDocumentReferenceValidation.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRDocumentPrinter.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRFrameParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRFunctionParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRFunctionRecordsParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRMetadataParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRMetadataTextParser.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRMetadataTextParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRParserUtilities.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIPrinter.cpp"
  ${RCCBackendMIRHeaders}
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/EmbeddedIR.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/LLVMConstantSyntax.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIDebugSyntax.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MILexer.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRDataParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRDocument.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRDocumentParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRDocumentReferenceValidation.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRDocumentPrinter.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRFrameParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRFunctionParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRFunctionRecordsParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRMetadataParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRMetadataTextParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIParser.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIPrinter.cpp"
)
