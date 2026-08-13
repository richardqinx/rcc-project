# SPDX-License-Identifier: GPL-3.0-or-later

set(RCCBackendTableGenSources Parser.cpp Record.cpp)
list(TRANSFORM RCCBackendTableGenSources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
set(
  RCCBackendTableGenHeaders
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/TableGen/Parser.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/TableGen/Record.h"
)

add_library(RCCBackendTableGen ${RCCBackendTableGenSources})
add_library(RCCBackend::TableGen ALIAS RCCBackendTableGen)
rcc_backend_configure_target(RCCBackendTableGen)
target_include_directories(
  RCCBackendTableGen
  PUBLIC "$<BUILD_INTERFACE:${RCC_BACKEND_SOURCE_DIR}/include>"
)
target_link_libraries(RCCBackendTableGen PUBLIC RCCBackend::Support)

rcc_backend_register_format_files(
  ${RCCBackendTableGenSources}
  ${RCCBackendTableGenHeaders}
)
rcc_backend_register_tidy_files(${RCCBackendTableGenSources})
