# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(RCCBackendEmbeddedIRTests EmbeddedIRTest.cpp)
rcc_backend_configure_target(RCCBackendEmbeddedIRTests)
target_link_libraries(RCCBackendEmbeddedIRTests PRIVATE RCCBackend::MIR)

add_test(NAME RCCBackend.MIR.EmbeddedIR COMMAND RCCBackendEmbeddedIRTests)

add_executable(RCCBackendMILexerTests MILexerTest.cpp)
rcc_backend_configure_target(RCCBackendMILexerTests)
target_link_libraries(RCCBackendMILexerTests PRIVATE RCCBackend::MIR)

add_test(NAME RCCBackend.MIR.MILexer COMMAND RCCBackendMILexerTests)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/EmbeddedIRTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MILexerTest.cpp"
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/EmbeddedIRTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MILexerTest.cpp"
)
