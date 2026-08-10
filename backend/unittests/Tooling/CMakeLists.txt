# SPDX-License-Identifier: GPL-3.0-or-later

function(rcc_backend_add_tooling_test target source test_name)
  add_executable(${target} ${source})
  rcc_backend_configure_target(${target})
  target_link_libraries(${target} PRIVATE RCCBackend::Tooling RCCBackend::RISCV)
  target_compile_definitions(
    ${target}
    PRIVATE
      RCC_BACKEND_TOOLING_TEST_INPUT_DIR="${RCC_BACKEND_SOURCE_DIR}/unittests/Target/Inputs"
  )
  add_test(NAME ${test_name} COMMAND ${target})
endfunction()

rcc_backend_add_tooling_test(
  RCCBackendManifestTests
  BackendManifestTest.cpp
  RCCBackend.Tooling.BackendManifest
)
rcc_backend_add_tooling_test(
  RCCBackendArtifactStoreTests
  ArtifactStoreTest.cpp
  RCCBackend.Tooling.ArtifactStore
)
rcc_backend_add_tooling_test(
  RCCBackendPipelineDriverTests
  PipelineDriverTest.cpp
  RCCBackend.Tooling.PipelineDriver
)
rcc_backend_add_tooling_test(
  RCCBackendMIRReducerFuzzerTests
  MIRReducerFuzzerTest.cpp
  RCCBackend.Tooling.MIRReducerFuzzer
)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/BackendManifestTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ArtifactStoreTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/PipelineDriverTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRReducerFuzzerTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ToolingTestSupport.h"
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/BackendManifestTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ArtifactStoreTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/PipelineDriverTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRReducerFuzzerTest.cpp"
)
