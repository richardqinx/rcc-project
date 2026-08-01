# SPDX-License-Identifier: GPL-3.0-or-later

set(RCCBackendSupportSources Error.cpp TextDiagnostic.cpp YAML.cpp)
set(
  RCCBackendSupportHeaders
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Support/Error.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Support/TextDiagnostic.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Support/YAML.h"
)

add_library(RCCBackendSupport ${RCCBackendSupportSources})
add_library(RCCBackend::Support ALIAS RCCBackendSupport)

rcc_backend_configure_target(RCCBackendSupport)

target_include_directories(
  RCCBackendSupport
  PUBLIC
    "$<BUILD_INTERFACE:${RCC_BACKEND_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

set_target_properties(
  RCCBackendSupport
  PROPERTIES
    EXPORT_NAME Support
    OUTPUT_NAME RCCBackendSupport
)

install(
  TARGETS RCCBackendSupport
  EXPORT RCCBackendTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/Error.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TextDiagnostic.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/YAML.cpp"
  ${RCCBackendSupportHeaders}
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/Error.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TextDiagnostic.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/YAML.cpp"
)
