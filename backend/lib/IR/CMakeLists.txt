# SPDX-License-Identifier: GPL-3.0-or-later

set(RCCBackendIRSources AsmWriter.cpp Constants.cpp DataLayout.cpp)
set(
  RCCBackendIRHeaders
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/IR/Constants.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/IR/DataLayout.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/IR/PassInstrumentation.h"
)

add_library(RCCBackendIR ${RCCBackendIRSources})
add_library(RCCBackend::IR ALIAS RCCBackendIR)

rcc_backend_configure_target(RCCBackendIR)

target_include_directories(
  RCCBackendIR
  PUBLIC
    "$<BUILD_INTERFACE:${RCC_BACKEND_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(RCCBackendIR PUBLIC RCCBackend::Support)

set_target_properties(
  RCCBackendIR
  PROPERTIES
    EXPORT_NAME IR
    OUTPUT_NAME RCCBackendIR
)

install(
  TARGETS RCCBackendIR
  EXPORT RCCBackendTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/AsmWriter.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Constants.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/DataLayout.cpp"
  ${RCCBackendIRHeaders}
)
