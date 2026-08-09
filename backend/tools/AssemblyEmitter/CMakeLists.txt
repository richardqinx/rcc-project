# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(RCCBackendAssemblyEmitter AssemblyEmitterMain.cpp)
rcc_backend_configure_target(RCCBackendAssemblyEmitter)
target_link_libraries(
  RCCBackendAssemblyEmitter
  PRIVATE
    RCCBackend::MIR
    RCCBackend::RISCV
)

set_target_properties(
  RCCBackendAssemblyEmitter
  PROPERTIES
    OUTPUT_NAME rcc-backend-asm
)

install(
  TARGETS RCCBackendAssemblyEmitter
  EXPORT RCCBackendTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

find_program(RCCBackendLLVMMC NAMES llvm-mc-20 llvm-mc)
foreach(InputName IN ITEMS RISCVGenericPipeline RISCVWideConstantPipeline RISCVDataPipeline)
  add_test(
    NAME RCCBackend.AssemblyEmitter.${InputName}
    COMMAND
      "${CMAKE_COMMAND}"
      "-DEMITTER=$<TARGET_FILE:RCCBackendAssemblyEmitter>"
      "-DINPUT=${RCC_BACKEND_SOURCE_DIR}/unittests/Target/Inputs/${InputName}.mir"
      "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/${InputName}.s"
      "-DLLVM_MC=${RCCBackendLLVMMC}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/AssemblyPipelineTest.cmake"
  )
endforeach()

rcc_backend_register_format_files("${CMAKE_CURRENT_SOURCE_DIR}/AssemblyEmitterMain.cpp")
rcc_backend_register_tidy_files("${CMAKE_CURRENT_SOURCE_DIR}/AssemblyEmitterMain.cpp")
