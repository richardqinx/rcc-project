# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED TOOL OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR
   NOT DEFINED OUTPUT_KIND)
  message(FATAL_ERROR "MIR tool output test is missing a required argument")
endif()

get_filename_component(OutputDirectory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${OutputDirectory}")
set(RepeatedOutput "${OUTPUT}.repeat")

execute_process(
  COMMAND "${TOOL}" "${INPUT}" -filetype "${OUTPUT_KIND}" -o "${OUTPUT}"
  RESULT_VARIABLE FirstResult
  ERROR_VARIABLE FirstDiagnostics
)
if(NOT FirstResult EQUAL 0)
  message(FATAL_ERROR "${OUTPUT_KIND} production failed: ${FirstDiagnostics}")
endif()
execute_process(
  COMMAND "${TOOL}" "${INPUT}" -filetype "${OUTPUT_KIND}" -o "${RepeatedOutput}"
  RESULT_VARIABLE SecondResult
  ERROR_VARIABLE SecondDiagnostics
)
if(NOT SecondResult EQUAL 0)
  message(FATAL_ERROR "repeated ${OUTPUT_KIND} production failed: ${SecondDiagnostics}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${OUTPUT}" "${RepeatedOutput}"
  RESULT_VARIABLE ComparisonResult
)
if(NOT ComparisonResult EQUAL 0)
  message(FATAL_ERROR "${OUTPUT_KIND} output is not deterministic")
endif()

if(OUTPUT_KIND STREQUAL "asm")
  file(READ "${OUTPUT}" Assembly)
  if(NOT Assembly MATCHES "\\.attribute arch, \"rv64gc\"" OR
     NOT Assembly MATCHES "jalr x0, x1, 0")
    message(FATAL_ERROR "assembly output is missing required RV64GC content")
  endif()
  if(INPUT MATCHES "RISCVDataPipeline" AND
     (NOT Assembly MATCHES "\\.LCPI[0-9]+_0:" OR
      NOT Assembly MATCHES "\\.LJTI[0-9]+_0:"))
    message(FATAL_ERROR "assembly output is missing constant-pool or jump-table data")
  endif()
  if(INPUT MATCHES "RISCVStackFramePipeline" AND
     (NOT Assembly MATCHES "addi x2, x2, -32" OR
      NOT Assembly MATCHES "addi x2, x2, 32"))
    message(FATAL_ERROR "assembly output does not balance its stack frame")
  endif()
  if(DEFINED LLVM_MC AND NOT LLVM_MC STREQUAL "LLVM_MC-NOTFOUND" AND
     NOT LLVM_MC STREQUAL "")
    execute_process(
      COMMAND "${LLVM_MC}" -triple=riscv64-unknown-linux-gnu -filetype=null "${OUTPUT}"
      RESULT_VARIABLE AssemblerResult
      ERROR_VARIABLE AssemblerDiagnostics
    )
    if(NOT AssemblerResult EQUAL 0)
      message(FATAL_ERROR "LLVM MC rejected MIR tool assembly: ${AssemblerDiagnostics}")
    endif()
  endif()
elseif(OUTPUT_KIND STREQUAL "obj")
  file(READ "${OUTPUT}" ObjectMagic OFFSET 0 LIMIT 4 HEX)
  if(NOT ObjectMagic STREQUAL "7f454c46")
    message(FATAL_ERROR "object output is not an ELF file")
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
      message(FATAL_ERROR "LLVM readelf rejected MIR tool object: ${ReadELFDiagnostics}")
    endif()
    if(NOT ReadELFOutput MATCHES "Type:[ \t]+REL" OR
       NOT ReadELFOutput MATCHES "Machine:[ \t]+RISC-V" OR
       NOT ReadELFOutput MATCHES "Flags:[ \t]+0x5" OR
       NOT ReadELFOutput MATCHES "\\.text" OR
       NOT ReadELFOutput MATCHES "\\.eh_frame" OR
       NOT ReadELFOutput MATCHES "\\.riscv\\.attributes")
      message(FATAL_ERROR "object output is missing required RV64GC ELF metadata")
    endif()
    if(INPUT MATCHES "RISCVDataPipeline" AND
       (NOT ReadELFOutput MATCHES "\\.rodata" OR
        NOT ReadELFOutput MATCHES "\\.rela\\.text" OR
        NOT ReadELFOutput MATCHES "R_RISCV_64"))
      message(FATAL_ERROR "data object is missing data or relocation sections")
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
      message(FATAL_ERROR "LLVM objdump rejected MIR tool object: ${ObjdumpDiagnostics}")
    endif()
    if(NOT ObjdumpOutput MATCHES "file format elf64-littleriscv" OR
       NOT ObjdumpOutput MATCHES "ret")
      message(FATAL_ERROR "object output does not contain RV64GC function code")
    endif()
    if(INPUT MATCHES "RISCVStackFramePipeline" AND
       (NOT ObjdumpOutput MATCHES "addi[ \t]+sp, sp, -0x20" OR
        NOT ObjdumpOutput MATCHES "addi[ \t]+sp, sp, 0x20"))
      message(FATAL_ERROR "stack-frame object does not balance its stack pointer")
    endif()
  endif()
else()
  message(FATAL_ERROR "MIR tool output test received an unknown output kind")
endif()

file(REMOVE "${OUTPUT}" "${RepeatedOutput}")
