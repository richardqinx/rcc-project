# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED EMITTER OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
  message(FATAL_ERROR "object pipeline test is missing a required path")
endif()

set(RepeatedOutput "${OUTPUT}.repeat")
execute_process(
  COMMAND "${EMITTER}" --input "${INPUT}" --filetype obj --output "${OUTPUT}"
  RESULT_VARIABLE FirstResult
  ERROR_VARIABLE FirstDiagnostics
)
if(NOT FirstResult EQUAL 0)
  message(FATAL_ERROR "object pipeline failed: ${FirstDiagnostics}")
endif()

execute_process(
  COMMAND "${EMITTER}" --input "${INPUT}" --filetype obj --output "${RepeatedOutput}"
  RESULT_VARIABLE SecondResult
  ERROR_VARIABLE SecondDiagnostics
)
if(NOT SecondResult EQUAL 0)
  message(FATAL_ERROR "repeated object pipeline failed: ${SecondDiagnostics}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUTPUT}" "${RepeatedOutput}"
  RESULT_VARIABLE ComparisonResult
)
if(NOT ComparisonResult EQUAL 0)
  message(FATAL_ERROR "object pipeline output is not deterministic")
endif()

file(READ "${OUTPUT}" ObjectMagic OFFSET 0 LIMIT 4 HEX)
if(NOT ObjectMagic STREQUAL "7f454c46")
  message(FATAL_ERROR "object pipeline did not produce an ELF file")
endif()

if(DEFINED LLVM_READELF AND NOT LLVM_READELF STREQUAL "LLVM_READELF-NOTFOUND" AND
   NOT LLVM_READELF STREQUAL "")
  execute_process(
    COMMAND "${LLVM_READELF}" -h -S -s -r "${OUTPUT}"
    RESULT_VARIABLE ReadELFResult
    OUTPUT_VARIABLE ReadELFOutput
    ERROR_VARIABLE ReadELFDiagnostics
  )
  if(NOT ReadELFResult EQUAL 0)
    message(FATAL_ERROR "LLVM readelf rejected generated object: ${ReadELFDiagnostics}")
  endif()
  if(NOT ReadELFOutput MATCHES "Type:[ \t]+REL" OR
     NOT ReadELFOutput MATCHES "Machine:[ \t]+RISC-V" OR
     NOT ReadELFOutput MATCHES "Flags:[ \t]+0x5" OR
     NOT ReadELFOutput MATCHES "\\.text" OR
     NOT ReadELFOutput MATCHES "\\.eh_frame" OR
     NOT ReadELFOutput MATCHES "\\.riscv\\.attributes")
    message(FATAL_ERROR "generated object is missing required RV64GC ELF metadata")
  endif()
  if(INPUT MATCHES "RISCVDataPipeline" AND
     (NOT ReadELFOutput MATCHES "\\.rodata" OR
      NOT ReadELFOutput MATCHES "\\.rela\\.text" OR
      NOT ReadELFOutput MATCHES "R_RISCV_64"))
    message(FATAL_ERROR "data pipeline object is missing data or relocation sections")
  endif()
endif()

if(DEFINED LLVM_OBJDUMP AND NOT LLVM_OBJDUMP STREQUAL "LLVM_OBJDUMP-NOTFOUND" AND
   NOT LLVM_OBJDUMP STREQUAL "")
  execute_process(
    COMMAND "${LLVM_OBJDUMP}" -dr --no-show-raw-insn "${OUTPUT}"
    RESULT_VARIABLE ObjdumpResult
    OUTPUT_VARIABLE ObjdumpOutput
    ERROR_VARIABLE ObjdumpDiagnostics
  )
  if(NOT ObjdumpResult EQUAL 0)
    message(FATAL_ERROR "LLVM objdump rejected generated object: ${ObjdumpDiagnostics}")
  endif()
  if(NOT ObjdumpOutput MATCHES "file format elf64-littleriscv" OR
     NOT ObjdumpOutput MATCHES "ret")
    message(FATAL_ERROR "generated object does not contain RV64GC function code")
  endif()
  if(INPUT MATCHES "RISCVStackFramePipeline" AND
     (NOT ObjdumpOutput MATCHES "addi[ \t]+sp, sp, -0x20" OR
      NOT ObjdumpOutput MATCHES "addi[ \t]+sp, sp, 0x20"))
    message(FATAL_ERROR "stack-frame object does not balance its stack pointer")
  endif()
endif()

file(REMOVE "${OUTPUT}" "${RepeatedOutput}")
