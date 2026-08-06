# SPDX-License-Identifier: GPL-3.0-or-later

set(
  RCCBackendDescriptionGeneratorCoreSources
  "${RCC_BACKEND_SOURCE_DIR}/lib/Target/Description/TargetDescriptionGenerator.cpp"
)
set(
  RCCBackendDescriptionGeneratorCoreHeaders
  "${RCC_BACKEND_SOURCE_DIR}/lib/Target/Description/TargetDescriptionGenerator.h"
)

add_library(
  RCCBackendDescriptionGeneratorCore
  STATIC
  ${RCCBackendDescriptionGeneratorCoreSources}
)

rcc_backend_configure_target(RCCBackendDescriptionGeneratorCore)

target_include_directories(
  RCCBackendDescriptionGeneratorCore
  PUBLIC "$<BUILD_INTERFACE:${RCC_BACKEND_SOURCE_DIR}/lib/Target/Description>"
)
target_link_libraries(RCCBackendDescriptionGeneratorCore PUBLIC RCCBackend::Support)

add_executable(RCCBackendDescriptionGenerator DescriptionGeneratorMain.cpp)
rcc_backend_configure_target(RCCBackendDescriptionGenerator)
target_link_libraries(RCCBackendDescriptionGenerator PRIVATE RCCBackendDescriptionGeneratorCore)
set_target_properties(RCCBackendDescriptionGenerator PROPERTIES OUTPUT_NAME rcc-backend-descgen)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/DescriptionGeneratorMain.cpp"
  ${RCCBackendDescriptionGeneratorCoreSources}
  ${RCCBackendDescriptionGeneratorCoreHeaders}
)
rcc_backend_register_tidy_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/DescriptionGeneratorMain.cpp"
  ${RCCBackendDescriptionGeneratorCoreSources}
)
