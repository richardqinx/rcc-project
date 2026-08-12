# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED TOOL OR NOT DEFINED INPUT OR NOT DEFINED WORK_DIRECTORY)
  message(FATAL_ERROR "MIR tool stage test is missing a required path")
endif()

file(REMOVE_RECURSE "${WORK_DIRECTORY}")
file(MAKE_DIRECTORY "${WORK_DIRECTORY}")

set(
  Stages
  prelegalizer-mir
  legalized-mir
  regbank-mir
  selected-mir
  post-ssa-mir
  post-ra-mir
  final-mir
)
set(
  StageOptions
  --verify
  -stop-after=legalizer
  -stop-after=regbankselect
  -stop-after=instruction-select
  -stop-after=twoaddressinstruction
  -stop-after=PBQPAllocator
  -stop-after=final-encodability-verifier
)
set(StageIndex 0)
foreach(Stage IN LISTS Stages)
  list(GET StageOptions ${StageIndex} StageOption)
  set(StageOutput "${WORK_DIRECTORY}/${Stage}.mir")
  set(RepeatedOutput "${WORK_DIRECTORY}/${Stage}.repeat.mir")
  set(CanonicalOutput "${WORK_DIRECTORY}/${Stage}.canonical.mir")
  execute_process(
    COMMAND "${TOOL}" "${INPUT}" "${StageOption}" -o "${StageOutput}"
    RESULT_VARIABLE StageResult
    ERROR_VARIABLE StageDiagnostics
  )
  if(NOT StageResult EQUAL 0)
    message(FATAL_ERROR "${Stage} production failed: ${StageDiagnostics}")
  endif()
  execute_process(
    COMMAND "${TOOL}" "${INPUT}" "${StageOption}" -o "${RepeatedOutput}"
    RESULT_VARIABLE RepeatedResult
    ERROR_VARIABLE RepeatedDiagnostics
  )
  if(NOT RepeatedResult EQUAL 0)
    message(FATAL_ERROR "repeated ${Stage} production failed: ${RepeatedDiagnostics}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${StageOutput}" "${RepeatedOutput}"
    RESULT_VARIABLE DeterminismResult
  )
  if(NOT DeterminismResult EQUAL 0)
    message(FATAL_ERROR "${Stage} output is not deterministic")
  endif()
  execute_process(
    COMMAND "${TOOL}" --verify "${StageOutput}" -o "${CanonicalOutput}"
    RESULT_VARIABLE VerifyResult
    ERROR_VARIABLE VerifyDiagnostics
  )
  if(NOT VerifyResult EQUAL 0)
    message(FATAL_ERROR "${Stage} output failed strict MIR reload: ${VerifyDiagnostics}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${StageOutput}" "${CanonicalOutput}"
    RESULT_VARIABLE CanonicalResult
  )
  if(NOT CanonicalResult EQUAL 0)
    message(FATAL_ERROR "${Stage} output is not canonical after strict reload")
  endif()
  math(EXPR StageIndex "${StageIndex} + 1")
endforeach()

set(LLVMContractOutput "${WORK_DIRECTORY}/llvm-contract.mir")
execute_process(
  COMMAND
    "${TOOL}" --verify "${INPUT}" -mtriple=riscv64
    -mattr=+m,+a,+f,+d,+c -relocation-model=pic -code-model=small -simplify-mir
    -verify-machineinstrs -o "${LLVMContractOutput}"
  RESULT_VARIABLE LLVMContractResult
  ERROR_VARIABLE LLVMContractDiagnostics
)
if(NOT LLVMContractResult EQUAL 0)
  message(FATAL_ERROR "LLVM llc-compatible invocation failed: ${LLVMContractDiagnostics}")
endif()
file(SIZE "${LLVMContractOutput}" LLVMContractOutputSize)
if(LLVMContractOutputSize EQUAL 0)
  message(FATAL_ERROR "LLVM llc-compatible invocation produced no MIR")
endif()

foreach(
  RemovedOption
  IN ITEMS
    --features=+m
    --output=-
)
  execute_process(
    COMMAND "${TOOL}" "${INPUT}" "${RemovedOption}"
    RESULT_VARIABLE RemovedOptionResult
    ERROR_VARIABLE RemovedOptionDiagnostics
  )
  if(RemovedOptionResult EQUAL 0 OR
     NOT RemovedOptionDiagnostics MATCHES "unknown command-line option")
    message(FATAL_ERROR "removed option ${RemovedOption} remains accepted")
  endif()
endforeach()

set(PassList "${WORK_DIRECTORY}/passes.txt")
execute_process(
  COMMAND "${TOOL}" --list-passes "${INPUT}" -o "${PassList}"
  RESULT_VARIABLE ListResult
  ERROR_VARIABLE ListDiagnostics
)
if(NOT ListResult EQUAL 0)
  message(FATAL_ERROR "pass listing failed: ${ListDiagnostics}")
endif()
file(READ "${PassList}" PassListText)
foreach(RequiredPass IN ITEMS legalizer regbankselect instruction-select prologepilog)
  if(NOT PassListText MATCHES "(^|\n)${RequiredPass}(\n|$)")
    message(FATAL_ERROR "canonical pass list is missing ${RequiredPass}")
  endif()
endforeach()

set(LegalizedOutput "${WORK_DIRECTORY}/run-pass.mir")
execute_process(
  COMMAND
    "${TOOL}" "${INPUT}" -run-pass legalizer -o
    "${LegalizedOutput}"
  RESULT_VARIABLE RunPassResult
  ERROR_VARIABLE RunPassDiagnostics
)
if(NOT RunPassResult EQUAL 0)
  message(FATAL_ERROR "single-pass execution failed: ${RunPassDiagnostics}")
endif()

set(RegBankOutput "${WORK_DIRECTORY}/passes.mir")
execute_process(
  COMMAND
    "${TOOL}" "${INPUT}" -passes legalizer,regbankselect -o
    "${RegBankOutput}"
  RESULT_VARIABLE PassesResult
  ERROR_VARIABLE PassesDiagnostics
)
if(NOT PassesResult EQUAL 0)
  message(FATAL_ERROR "explicit pass pipeline failed: ${PassesDiagnostics}")
endif()

set(StoppedOutput "${WORK_DIRECTORY}/stopped.mir")
execute_process(
  COMMAND
    "${TOOL}" "${INPUT}" -stop-after legalizer -o
    "${StoppedOutput}"
  RESULT_VARIABLE StopResult
  ERROR_VARIABLE StopDiagnostics
)
if(NOT StopResult EQUAL 0)
  message(FATAL_ERROR "stop-after control failed: ${StopDiagnostics}")
endif()
execute_process(
  COMMAND "${TOOL}" --verify "${StoppedOutput}"
  RESULT_VARIABLE StopVerifyResult
  ERROR_VARIABLE StopVerifyDiagnostics
)
if(NOT StopVerifyResult EQUAL 0)
  message(FATAL_ERROR "stop-after output failed strict reload: ${StopVerifyDiagnostics}")
endif()

execute_process(
  COMMAND
    "${TOOL}" "${INPUT}" -filetype=null -print-before legalizer -print-after legalizer
  RESULT_VARIABLE PrintResult
  ERROR_VARIABLE PrintDiagnostics
)
if(NOT PrintResult EQUAL 0)
  message(FATAL_ERROR "pass snapshot controls failed: ${PrintDiagnostics}")
endif()
if(NOT PrintDiagnostics MATCHES "[*][*][*] prelegalizer-mir legalizer#1" OR
   NOT PrintDiagnostics MATCHES "[*][*][*] legalized-mir legalizer#1")
  message(FATAL_ERROR "before/after pass snapshots are incomplete")
endif()

execute_process(
  COMMAND
    "${TOOL}" "${INPUT}" -filetype=null --stage-snapshots
  RESULT_VARIABLE SnapshotResult
  ERROR_VARIABLE SnapshotDiagnostics
)
if(NOT SnapshotResult EQUAL 0)
  message(FATAL_ERROR "stage snapshot execution failed: ${SnapshotDiagnostics}")
endif()
foreach(Stage IN LISTS Stages)
  if(NOT SnapshotDiagnostics MATCHES "[*][*][*] ${Stage}")
    message(FATAL_ERROR "stage snapshots are missing ${Stage}")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIRECTORY}")
