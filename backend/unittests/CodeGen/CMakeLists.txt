# SPDX-License-Identifier: GPL-3.0-or-later

set(RCCBackendCodeGenTestSources CoreTypesTest.cpp)

add_executable(RCCBackendCodeGenTests ${RCCBackendCodeGenTestSources})
rcc_backend_configure_target(RCCBackendCodeGenTests)
target_link_libraries(RCCBackendCodeGenTests PRIVATE RCCBackend::CodeGen)

add_test(NAME RCCBackend.CodeGen.CoreTypes COMMAND RCCBackendCodeGenTests)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/CoreTypesTest.cpp"
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/CoreTypesTest.cpp"
)
