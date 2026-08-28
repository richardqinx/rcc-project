# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(RCCBackendSupportTests ErrorTest.cpp)
rcc_backend_configure_target(RCCBackendSupportTests)
target_link_libraries(RCCBackendSupportTests PRIVATE RCCBackend::Support)

add_test(NAME RCCBackend.Support.Error COMMAND RCCBackendSupportTests)

add_executable(RCCBackendTextDiagnosticTests TextDiagnosticTest.cpp)
rcc_backend_configure_target(RCCBackendTextDiagnosticTests)
target_link_libraries(RCCBackendTextDiagnosticTests PRIVATE RCCBackend::Support)

add_test(NAME RCCBackend.Support.TextDiagnostic COMMAND RCCBackendTextDiagnosticTests)

add_executable(RCCBackendYAMLTests YAMLTest.cpp)
rcc_backend_configure_target(RCCBackendYAMLTests)
target_link_libraries(RCCBackendYAMLTests PRIVATE RCCBackend::Support)

add_test(NAME RCCBackend.Support.YAML COMMAND RCCBackendYAMLTests)

add_executable(RCCBackendSHA256Tests SHA256Test.cpp)
rcc_backend_configure_target(RCCBackendSHA256Tests)
target_link_libraries(RCCBackendSHA256Tests PRIVATE RCCBackend::Support)

add_test(NAME RCCBackend.Support.SHA256 COMMAND RCCBackendSHA256Tests)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/ErrorTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/SHA256Test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TextDiagnosticTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/YAMLTest.cpp"
)
