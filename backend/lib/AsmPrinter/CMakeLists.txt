# SPDX-License-Identifier: GPL-3.0-or-later

set(
  RCCBackendAsmPrinterSources
  AsmPrinter.cpp
  MachineMCContext.cpp
  TargetMCInstLowering.cpp
)
set(
  RCCBackendAsmPrinterHeaders
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/AsmPrinter.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/MachineMCContext.h"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/CodeGen/TargetMCInstLowering.h"
)

add_library(RCCBackendAsmPrinter ${RCCBackendAsmPrinterSources})
add_library(RCCBackend::AsmPrinter ALIAS RCCBackendAsmPrinter)

rcc_backend_configure_target(RCCBackendAsmPrinter)

target_include_directories(
  RCCBackendAsmPrinter
  PUBLIC
    "$<BUILD_INTERFACE:${RCC_BACKEND_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(
  RCCBackendAsmPrinter
  PUBLIC
    RCCBackend::CodeGen
    RCCBackend::MC
)

set_target_properties(
  RCCBackendAsmPrinter
  PROPERTIES
    EXPORT_NAME AsmPrinter
    OUTPUT_NAME RCCBackendAsmPrinter
)

install(
  TARGETS RCCBackendAsmPrinter
  EXPORT RCCBackendTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

set(RCCBackendAsmPrinterSourcePaths)
foreach(Source IN LISTS RCCBackendAsmPrinterSources)
  list(APPEND RCCBackendAsmPrinterSourcePaths "${CMAKE_CURRENT_SOURCE_DIR}/${Source}")
endforeach()

rcc_backend_register_format_files(
  ${RCCBackendAsmPrinterSourcePaths}
  ${RCCBackendAsmPrinterHeaders}
)
rcc_backend_register_tidy_files(${RCCBackendAsmPrinterSourcePaths})
