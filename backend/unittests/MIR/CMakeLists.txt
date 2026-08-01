# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(RCCBackendEmbeddedIRTests EmbeddedIRTest.cpp)
rcc_backend_configure_target(RCCBackendEmbeddedIRTests)
target_link_libraries(RCCBackendEmbeddedIRTests PRIVATE RCCBackend::MIR)

add_test(NAME RCCBackend.MIR.EmbeddedIR COMMAND RCCBackendEmbeddedIRTests)

rcc_backend_register_format_files("${CMAKE_CURRENT_SOURCE_DIR}/EmbeddedIRTest.cpp")
rcc_backend_register_tidy_files("${CMAKE_CURRENT_SOURCE_DIR}/EmbeddedIRTest.cpp")
