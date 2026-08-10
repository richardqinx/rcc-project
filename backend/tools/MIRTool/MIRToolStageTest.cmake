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
foreach(Stage IN LISTS Stages)
  set(StageOutput "${WORK_DIRECTORY}/${Stage}.mir")
  set(RepeatedOutput "${WORK_DIRECTORY}/${Stage}.repeat.mir")
  set(CanonicalOutput "${WORK_DIRECTORY}/${Stage}.canonical.mir")
  execute_process(
    COMMAND "${TOOL}" --input "${INPUT}" --emit "${Stage}" --output "${StageOutput}"
    RESULT_VARIABLE StageResult
    ERROR_VARIABLE StageDiagnostics
  )
  if(NOT StageResult EQUAL 0)
    message(FATAL_ERROR "${Stage} production failed: ${StageDiagnostics}")
  endif()
  execute_process(
    COMMAND "${TOOL}" --input "${INPUT}" --emit "${Stage}" --output "${RepeatedOutput}"
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
    COMMAND "${TOOL}" --verify --input "${StageOutput}" --output "${CanonicalOutput}"
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
endforeach()

set(PassList "${WORK_DIRECTORY}/passes.txt")
execute_process(
  COMMAND "${TOOL}" --list-passes --input "${INPUT}" --output "${PassList}"
  RESULT_VARIABLE ListResult
  ERROR_VARIABLE ListDiagnostics
)
if(NOT ListResult EQUAL 0)
  message(FATAL_ERROR "pass listing failed: ${ListDiagnostics}")
endif()
file(READ "${PassList}" PassListText)
foreach(RequiredPass IN ITEMS Legalizer RegisterBankSelect InstructionSelect PrologueEpilogueInsertion)
  if(NOT PassListText MATCHES "(^|\n)${RequiredPass}(\n|$)")
    message(FATAL_ERROR "canonical pass list is missing ${RequiredPass}")
  endif()
endforeach()

set(LegalizedOutput "${WORK_DIRECTORY}/run-pass.mir")
execute_process(
  COMMAND
    "${TOOL}" --input "${INPUT}" --run-pass Legalizer --emit legalized-mir --output
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
    "${TOOL}" --input "${INPUT}" --passes Legalizer,RegisterBankSelect --emit regbank-mir
    --output "${RegBankOutput}"
  RESULT_VARIABLE PassesResult
  ERROR_VARIABLE PassesDiagnostics
)
if(NOT PassesResult EQUAL 0)
  message(FATAL_ERROR "explicit pass pipeline failed: ${PassesDiagnostics}")
endif()

set(StoppedOutput "${WORK_DIRECTORY}/stopped.mir")
execute_process(
  COMMAND
    "${TOOL}" --input "${INPUT}" --stop-after Legalizer --emit final-mir --output
    "${StoppedOutput}"
  RESULT_VARIABLE StopResult
  ERROR_VARIABLE StopDiagnostics
)
if(NOT StopResult EQUAL 0)
  message(FATAL_ERROR "stop-after control failed: ${StopDiagnostics}")
endif()
execute_process(
  COMMAND "${TOOL}" --verify --input "${StoppedOutput}"
  RESULT_VARIABLE StopVerifyResult
  ERROR_VARIABLE StopVerifyDiagnostics
)
if(NOT StopVerifyResult EQUAL 0)
  message(FATAL_ERROR "stop-after output failed strict reload: ${StopVerifyDiagnostics}")
endif()

execute_process(
  COMMAND
    "${TOOL}" --input "${INPUT}" --emit legalized-mir --print-before Legalizer --print-after
    Legalizer --output "${WORK_DIRECTORY}/printed.mir"
  RESULT_VARIABLE PrintResult
  ERROR_VARIABLE PrintDiagnostics
)
if(NOT PrintResult EQUAL 0)
  message(FATAL_ERROR "pass snapshot controls failed: ${PrintDiagnostics}")
endif()
if(NOT PrintDiagnostics MATCHES "[*][*][*] prelegalizer-mir Legalizer#1" OR
   NOT PrintDiagnostics MATCHES "[*][*][*] legalized-mir Legalizer#1")
  message(FATAL_ERROR "before/after pass snapshots are incomplete")
endif()

execute_process(
  COMMAND
    "${TOOL}" --input "${INPUT}" --emit final-mir --stage-snapshots --output
    "${WORK_DIRECTORY}/snapshot-final.mir"
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
