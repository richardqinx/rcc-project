# SPDX-License-Identifier: GPL-3.0-or-later

set(
  RCCBackendMCSources
  MCCFI.cpp
  MCContext.cpp
  MCExpr.cpp
  MCInstPrinter.cpp
  MCSection.cpp
  MCStreamer.cpp
)
set(
  RCCBackendMCHeaders
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCCFI.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCContext.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCExpr.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCInst.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCInstPrinter.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCSection.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCStreamer.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCSymbol.h"
)

add_library(RCCBackendMC ${RCCBackendMCSources})
add_library(RCCBackend::MC ALIAS RCCBackendMC)

rcc_backend_configure_target(RCCBackendMC)

target_include_directories(
  RCCBackendMC
  PUBLIC
    "$<BUILD_INTERFACE:${RCC_BACKEND_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(RCCBackendMC PUBLIC RCCBackend::Support)

set_target_properties(
  RCCBackendMC
  PROPERTIES
    EXPORT_NAME MC
    OUTPUT_NAME RCCBackendMC
)

install(
  TARGETS RCCBackendMC
  EXPORT RCCBackendTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

set(RCCBackendMCSourcePaths)
foreach(Source IN LISTS RCCBackendMCSources)
  list(APPEND RCCBackendMCSourcePaths "${CMAKE_CURRENT_SOURCE_DIR}/${Source}")
endforeach()

rcc_backend_register_format_files(${RCCBackendMCSourcePaths} ${RCCBackendMCHeaders})
rcc_backend_register_tidy_files(${RCCBackendMCSourcePaths})
