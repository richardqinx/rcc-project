# SPDX-License-Identifier: GPL-3.0-or-later

add_library(RCCBackendTestTarget TestTarget.cpp)
add_library(RCCBackend::TestTarget ALIAS RCCBackendTestTarget)

rcc_backend_configure_target(RCCBackendTestTarget)
target_include_directories(
  RCCBackendTestTarget
  PUBLIC "${RCC_BACKEND_SOURCE_DIR}/unittests"
)
target_link_libraries(RCCBackendTestTarget PUBLIC RCCBackend::CodeGen)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/TestTarget.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TestTarget.h"
)
rcc_backend_register_tidy_files("${CMAKE_CURRENT_SOURCE_DIR}/TestTarget.cpp")
