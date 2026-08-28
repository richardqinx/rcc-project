# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(RCCBackendLLParserTests LLParserTest.cpp)
rcc_backend_configure_target(RCCBackendLLParserTests)
target_link_libraries(
  RCCBackendLLParserTests
  PRIVATE
    RCCBackend::AsmParser
    RCCBackend::IR
)

add_test(NAME RCCBackend.AsmParser.LLParser COMMAND RCCBackendLLParserTests)

rcc_backend_register_format_files("${CMAKE_CURRENT_SOURCE_DIR}/LLParserTest.cpp")
