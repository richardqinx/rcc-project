# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(RCCBackendMCTests MCTest.cpp)
rcc_backend_configure_target(RCCBackendMCTests)
target_link_libraries(RCCBackendMCTests PRIVATE RCCBackend::MC)
add_test(NAME RCCBackend.MC.Core COMMAND RCCBackendMCTests)

rcc_backend_register_format_files("${CMAKE_CURRENT_SOURCE_DIR}/MCTest.cpp")
rcc_backend_register_tidy_files("${CMAKE_CURRENT_SOURCE_DIR}/MCTest.cpp")
