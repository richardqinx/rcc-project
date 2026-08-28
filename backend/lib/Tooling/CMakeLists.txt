# SPDX-License-Identifier: GPL-3.0-or-later

set(
  RCCBackendToolingSources
  BackendInvocation.cpp
  BackendManifest.cpp
  BackendDriver.cpp
  ArtifactStore.cpp
  MIRFuzzer.cpp
  MIRReducer.cpp
  PipelineDriver.cpp
)
set(
  RCCBackendToolingHeaders
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Tooling/BackendInvocation.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Tooling/BackendManifest.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Tooling/BackendDriver.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Tooling/ArtifactStore.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Tooling/MIRFuzzer.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Tooling/MIRReducer.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Tooling/PipelineDriver.h"
)

add_library(RCCBackendTooling ${RCCBackendToolingSources})
add_library(RCCBackend::Tooling ALIAS RCCBackendTooling)

rcc_backend_configure_target(RCCBackendTooling)

target_include_directories(
  RCCBackendTooling
  PUBLIC
    "$<BUILD_INTERFACE:${RCC_BACKEND_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(
  RCCBackendTooling
  PUBLIC
    RCCBackend::MIR
    RCCBackend::MC
    RCCBackend::Support
    RCCBackend::Target
)

set_target_properties(
  RCCBackendTooling
  PROPERTIES
    EXPORT_NAME Tooling
    OUTPUT_NAME RCCBackendTooling
)

install(
  TARGETS RCCBackendTooling
  EXPORT RCCBackendTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/BackendInvocation.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/BackendManifest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/BackendDriver.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ArtifactStore.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRFuzzer.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/MIRReducer.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/PipelineDriver.cpp"
  ${RCCBackendToolingHeaders}
)
