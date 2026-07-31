# SPDX-License-Identifier: GPL-3.0-or-later

set(RCCBackendSupportTestSources ErrorTest.cpp)

add_executable(RCCBackendSupportTests ${RCCBackendSupportTestSources})
rcc_backend_configure_target(RCCBackendSupportTests)
target_link_libraries(RCCBackendSupportTests PRIVATE RCCBackend::Support)

add_test(NAME RCCBackend.Support.Error COMMAND RCCBackendSupportTests)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/ErrorTest.cpp"
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/ErrorTest.cpp"
)
