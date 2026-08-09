# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(RCCBackendMCTests MCTest.cpp)
rcc_backend_configure_target(RCCBackendMCTests)
target_link_libraries(RCCBackendMCTests PRIVATE RCCBackend::MC)
add_test(NAME RCCBackend.MC.Core COMMAND RCCBackendMCTests)

add_executable(RCCBackendMCObjectTests MCObjectTest.cpp)
rcc_backend_configure_target(RCCBackendMCObjectTests)
target_link_libraries(RCCBackendMCObjectTests PRIVATE RCCBackend::MC)
add_test(NAME RCCBackend.MC.Object COMMAND RCCBackendMCObjectTests)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/MCTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MCObjectTest.cpp"
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/MCTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MCObjectTest.cpp"
)
