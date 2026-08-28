# SPDX-License-Identifier: GPL-3.0-or-later

set(
  RCCBackendMCSources
  ELFObjectWriter.cpp
  MCCFI.cpp
  MCDwarfFrameEmitter.cpp
  MCAsmBackend.cpp
  MCAssembler.cpp
  MCContext.cpp
  MCExpr.cpp
  MCFixup.cpp
  MCInstPrinter.cpp
  MCObjectStreamer.cpp
  MCSection.cpp
  MCStreamer.cpp
  TargetRegistry.cpp
)
set(
  RCCBackendMCHeaders
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/ELFObjectWriter.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCCFI.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCDwarfFrameEmitter.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCAsmBackend.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCAssembler.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCCodeEmitter.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCContext.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCExpr.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCFixup.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCInst.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCInstPrinter.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCObjectStreamer.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCObjectWriter.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCSection.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCStreamer.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/MCSymbol.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/MC/TargetRegistry.h"
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
target_link_libraries(RCCBackendMC PUBLIC RCCBackend::Support RCCBackend::Target)

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
