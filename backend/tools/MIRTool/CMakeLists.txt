# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(RCCBackendMIRTool MIRToolMain.cpp)
rcc_backend_configure_target(RCCBackendMIRTool)
target_link_libraries(
  RCCBackendMIRTool
  PRIVATE
    RCCBackend::Tooling
    RCCBackend::RISCV
)

set_target_properties(
  RCCBackendMIRTool
  PROPERTIES
    OUTPUT_NAME rcc-backend-mir
)

install(
  TARGETS RCCBackendMIRTool
  EXPORT RCCBackendTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

find_program(RCCBackendMIRToolLLVMMC NAMES llvm-mc-20 llvm-mc)
find_program(RCCBackendMIRToolLLVMReadELF NAMES llvm-readelf-20 llvm-readelf)
find_program(RCCBackendMIRToolLLVMObjdump NAMES llvm-objdump-20 llvm-objdump)

set(RCCBackendMIRToolInputDirectory "${RCC_BACKEND_SOURCE_DIR}/unittests/Target/Inputs")
set(RCCBackendMIRToolTestDirectory "${CMAKE_CURRENT_BINARY_DIR}/tests")

add_test(
  NAME RCCBackend.MIRTool.StagesAndPasses
  COMMAND
    "${CMAKE_COMMAND}"
    "-DTOOL=$<TARGET_FILE:RCCBackendMIRTool>"
    "-DINPUT=${RCCBackendMIRToolInputDirectory}/RISCVGenericPipeline.mir"
    "-DWORK_DIRECTORY=${RCCBackendMIRToolTestDirectory}/stages"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/MIRToolStageTest.cmake"
)

foreach(
  InputName
  IN ITEMS
    RISCVGenericPipeline
    RISCVWideConstantPipeline
    RISCVDataPipeline
    RISCVStackFramePipeline
)
  add_test(
    NAME RCCBackend.MIRTool.Assembly.${InputName}
    COMMAND
      "${CMAKE_COMMAND}"
      "-DTOOL=$<TARGET_FILE:RCCBackendMIRTool>"
      "-DINPUT=${RCCBackendMIRToolInputDirectory}/${InputName}.mir"
      "-DOUTPUT=${RCCBackendMIRToolTestDirectory}/${InputName}.s"
      -DOUTPUT_KIND=asm
      "-DLLVM_MC=${RCCBackendMIRToolLLVMMC}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/MIRToolOutputTest.cmake"
  )
  add_test(
    NAME RCCBackend.MIRTool.Object.${InputName}
    COMMAND
      "${CMAKE_COMMAND}"
      "-DTOOL=$<TARGET_FILE:RCCBackendMIRTool>"
      "-DINPUT=${RCCBackendMIRToolInputDirectory}/${InputName}.mir"
      "-DOUTPUT=${RCCBackendMIRToolTestDirectory}/${InputName}.o"
      -DOUTPUT_KIND=obj
      "-DLLVM_READELF=${RCCBackendMIRToolLLVMReadELF}"
      "-DLLVM_OBJDUMP=${RCCBackendMIRToolLLVMObjdump}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/MIRToolOutputTest.cmake"
  )
endforeach()

add_test(
  NAME RCCBackend.MIRTool.Operations
  COMMAND
    "${CMAKE_COMMAND}"
    "-DTOOL=$<TARGET_FILE:RCCBackendMIRTool>"
    "-DINPUT=${RCCBackendMIRToolInputDirectory}/RISCVGenericPipeline.mir"
    "-DWORK_DIRECTORY=${RCCBackendMIRToolTestDirectory}/operations"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/MIRToolOperationsTest.cmake"
)

set_tests_properties(
  RCCBackend.MIRTool.StagesAndPasses
  RCCBackend.MIRTool.Operations
  PROPERTIES TIMEOUT 180
)

rcc_backend_register_format_files("${CMAKE_CURRENT_SOURCE_DIR}/MIRToolMain.cpp")
