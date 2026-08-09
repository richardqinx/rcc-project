# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED EMITTER OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
  message(FATAL_ERROR "assembly pipeline test is missing a required path")
endif()

execute_process(
  COMMAND "${EMITTER}" --input "${INPUT}"
  RESULT_VARIABLE FirstResult
  OUTPUT_VARIABLE FirstAssembly
  ERROR_VARIABLE FirstDiagnostics
)
if(NOT FirstResult EQUAL 0)
  message(FATAL_ERROR "assembly pipeline failed: ${FirstDiagnostics}")
endif()
if(NOT FirstAssembly MATCHES "\\.attribute arch, \"rv64gc\"" OR
   NOT FirstAssembly MATCHES "jalr x0, x1, 0")
  message(FATAL_ERROR "assembly pipeline output is missing required RV64GC content")
endif()
if(INPUT MATCHES "RISCVDataPipeline" AND
   (NOT FirstAssembly MATCHES "\\.LCPI[0-9]+_0:" OR
    NOT FirstAssembly MATCHES "\\.LJTI[0-9]+_0:"))
  message(FATAL_ERROR "assembly pipeline did not emit its constant pool and jump table")
endif()

execute_process(
  COMMAND "${EMITTER}" --input "${INPUT}"
  RESULT_VARIABLE SecondResult
  OUTPUT_VARIABLE SecondAssembly
  ERROR_VARIABLE SecondDiagnostics
)
if(NOT SecondResult EQUAL 0)
  message(FATAL_ERROR "repeated assembly pipeline failed: ${SecondDiagnostics}")
endif()
if(NOT FirstAssembly STREQUAL SecondAssembly)
  message(FATAL_ERROR "assembly pipeline output is not deterministic")
endif()

if(DEFINED LLVM_MC AND NOT LLVM_MC STREQUAL "LLVM_MC-NOTFOUND" AND
   NOT LLVM_MC STREQUAL "")
  file(WRITE "${OUTPUT}" "${FirstAssembly}")
  execute_process(
    COMMAND "${LLVM_MC}" -triple=riscv64-unknown-linux-gnu -filetype=null "${OUTPUT}"
    RESULT_VARIABLE AssemblerResult
    OUTPUT_VARIABLE AssemblerOutput
    ERROR_VARIABLE AssemblerDiagnostics
  )
  file(REMOVE "${OUTPUT}")
  if(NOT AssemblerResult EQUAL 0)
    message(FATAL_ERROR "LLVM MC rejected generated assembly: ${AssemblerDiagnostics}")
  endif()
endif()
