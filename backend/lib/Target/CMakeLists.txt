# SPDX-License-Identifier: GPL-3.0-or-later

add_library(RCCBackendTarget TargetMachine.cpp)
add_library(RCCBackend::Target ALIAS RCCBackendTarget)

rcc_backend_configure_target(RCCBackendTarget)

target_include_directories(
  RCCBackendTarget
  PUBLIC
    "$<BUILD_INTERFACE:${RCC_BACKEND_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(RCCBackendTarget PUBLIC RCCBackend::CodeGen)

set_target_properties(
  RCCBackendTarget
  PROPERTIES
    EXPORT_NAME Target
    OUTPUT_NAME RCCBackendTarget
)

install(
  TARGETS RCCBackendTarget
  EXPORT RCCBackendTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/TargetMachine.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/TargetMachine.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/TargetOptions.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Support/TargetSelect.h"
)
rcc_backend_register_tidy_files("${CMAKE_CURRENT_SOURCE_DIR}/TargetMachine.cpp")

add_subdirectory(RISCV)
