# SPDX-License-Identifier: GPL-3.0-or-later

set(RCCBackendRISCVDescription "${CMAKE_CURRENT_SOURCE_DIR}/RISCV.rccdesc")
set(
  RCCBackendRISCVGeneratedHeader
  "${RCC_BACKEND_BINARY_DIR}/generated/include/rcc/Target/RISCV/RISCVRegisters.h"
)
set(
  RCCBackendRISCVGeneratedTables
  "${RCC_BACKEND_BINARY_DIR}/generated/lib/Target/RISCV/RISCVGenTargetDescription.inc"
)

add_custom_command(
  OUTPUT "${RCCBackendRISCVGeneratedHeader}" "${RCCBackendRISCVGeneratedTables}"
  COMMAND
    "$<TARGET_FILE:RCCBackendDescriptionGenerator>"
    --input "${RCCBackendRISCVDescription}"
    --header "${RCCBackendRISCVGeneratedHeader}"
    --tables "${RCCBackendRISCVGeneratedTables}"
  DEPENDS RCCBackendDescriptionGenerator "${RCCBackendRISCVDescription}"
  COMMENT "Generating RCC RV64GC target tables"
  VERBATIM
)

add_custom_target(
  RCCBackendRISCVGeneratedTablesTarget
  DEPENDS "${RCCBackendRISCVGeneratedHeader}" "${RCCBackendRISCVGeneratedTables}"
)

set_source_files_properties(
  "${RCCBackendRISCVGeneratedHeader}" "${RCCBackendRISCVGeneratedTables}"
  PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE
)

add_library(
  RCCBackendRISCV
  RISCVTarget.cpp
  "${RCCBackendRISCVGeneratedHeader}"
  "${RCCBackendRISCVGeneratedTables}"
)
add_library(RCCBackend::RISCV ALIAS RCCBackendRISCV)
add_dependencies(RCCBackendRISCV RCCBackendRISCVGeneratedTablesTarget)

rcc_backend_configure_target(RCCBackendRISCV)

target_include_directories(
  RCCBackendRISCV
  PUBLIC
    "$<BUILD_INTERFACE:${RCC_BACKEND_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${RCC_BACKEND_BINARY_DIR}/generated/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE "${RCC_BACKEND_BINARY_DIR}/generated/lib/Target/RISCV"
)
target_link_libraries(RCCBackendRISCV PUBLIC RCCBackend::CodeGen)

set_target_properties(
  RCCBackendRISCV
  PROPERTIES
    EXPORT_NAME RISCV
    OUTPUT_NAME RCCBackendRISCV
)

install(
  TARGETS RCCBackendRISCV
  EXPORT RCCBackendTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
  FILES "${RCCBackendRISCVGeneratedHeader}"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/rcc/Target/RISCV"
)

rcc_backend_register_format_files(
  "${CMAKE_CURRENT_SOURCE_DIR}/RISCVTarget.cpp"
  "${RCC_BACKEND_SOURCE_DIR}/include/rcc/Target/RISCV/RISCVTarget.h"
)
rcc_backend_register_tidy_files("${CMAKE_CURRENT_SOURCE_DIR}/RISCVTarget.cpp")
