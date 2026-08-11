# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED TOOL OR NOT DEFINED INPUT OR NOT DEFINED WORK_DIRECTORY)
  message(FATAL_ERROR "MIR tool operations test is missing a required path")
endif()

file(REMOVE_RECURSE "${WORK_DIRECTORY}")
file(MAKE_DIRECTORY "${WORK_DIRECTORY}")

set(StandardInputMIR "${WORK_DIRECTORY}/stdin.mir")
execute_process(
  COMMAND "${TOOL}" --verify - -o "${StandardInputMIR}"
  INPUT_FILE "${INPUT}"
  RESULT_VARIABLE StandardInputResult
  ERROR_VARIABLE StandardInputDiagnostics
)
if(NOT StandardInputResult EQUAL 0)
  message(FATAL_ERROR "standard-input MIR parsing failed: ${StandardInputDiagnostics}")
endif()
file(SIZE "${StandardInputMIR}" StandardInputSize)
if(StandardInputSize EQUAL 0)
  message(FATAL_ERROR "standard-input MIR parsing produced no canonical output")
endif()

set(CacheDirectory "${WORK_DIRECTORY}/cache")
set(CachedAssembly "${WORK_DIRECTORY}/cached.s")
set(ReloadedAssembly "${WORK_DIRECTORY}/reloaded.s")
execute_process(
  COMMAND
    "${TOOL}" --input "${INPUT}" --emit assembly --cache-dir "${CacheDirectory}"
    --cache-policy write -o "${CachedAssembly}"
  RESULT_VARIABLE CacheWriteResult
  ERROR_VARIABLE CacheWriteDiagnostics
)
if(NOT CacheWriteResult EQUAL 0)
  message(FATAL_ERROR "cache write failed: ${CacheWriteDiagnostics}")
endif()
execute_process(
  COMMAND
    "${TOOL}" --input "${INPUT}" --emit assembly --cache-dir "${CacheDirectory}"
    --cache-policy read -o "${ReloadedAssembly}"
  RESULT_VARIABLE CacheReadResult
  ERROR_VARIABLE CacheReadDiagnostics
)
if(NOT CacheReadResult EQUAL 0)
  message(FATAL_ERROR "cache hit reload failed: ${CacheReadDiagnostics}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${CachedAssembly}" "${ReloadedAssembly}"
  RESULT_VARIABLE CacheComparisonResult
)
if(NOT CacheComparisonResult EQUAL 0)
  message(FATAL_ERROR "cache hit changed the artifact bytes")
endif()

execute_process(
  COMMAND
    "${TOOL}" --input "${INPUT}" --emit assembly --cache-dir
    "${WORK_DIRECTORY}/empty-cache" --cache-policy read -o "${WORK_DIRECTORY}/miss.s"
  RESULT_VARIABLE CacheMissResult
  ERROR_VARIABLE CacheMissDiagnostics
)
if(CacheMissResult EQUAL 0 OR NOT CacheMissDiagnostics MATCHES "cache miss")
  message(FATAL_ERROR "strict cache read did not report a cache miss")
endif()

file(GLOB_RECURSE CachedArtifacts "${CacheDirectory}/artifact.s")
list(LENGTH CachedArtifacts CachedArtifactCount)
if(NOT CachedArtifactCount EQUAL 1)
  message(FATAL_ERROR "cache publication did not create exactly one assembly artifact")
endif()
list(GET CachedArtifacts 0 CachedArtifact)
file(WRITE "${CachedArtifact}" "corrupt")
execute_process(
  COMMAND
    "${TOOL}" --input "${INPUT}" --emit assembly --cache-dir "${CacheDirectory}"
    --cache-policy read -o "${WORK_DIRECTORY}/corrupt.s"
  RESULT_VARIABLE CorruptCacheResult
  ERROR_VARIABLE CorruptCacheDiagnostics
)
if(CorruptCacheResult EQUAL 0 OR NOT CorruptCacheDiagnostics MATCHES "cache miss")
  message(FATAL_ERROR "strict cache reload accepted a corrupted artifact")
endif()

set(SuccessReproducer "${WORK_DIRECTORY}/success-reproducer")
set(SuccessAssembly "${WORK_DIRECTORY}/success.s")
set(ReplayedAssembly "${WORK_DIRECTORY}/success-replayed.s")
execute_process(
  COMMAND
    "${TOOL}" --input "${INPUT}" --emit assembly --crash-reproducer "${SuccessReproducer}"
    -o "${SuccessAssembly}"
  RESULT_VARIABLE ReproducerWriteResult
  ERROR_VARIABLE ReproducerWriteDiagnostics
)
if(NOT ReproducerWriteResult EQUAL 0)
  message(FATAL_ERROR "successful reproducer publication failed: ${ReproducerWriteDiagnostics}")
endif()
execute_process(
  COMMAND "${TOOL}" --reproduce "${SuccessReproducer}" -o "${ReplayedAssembly}"
  RESULT_VARIABLE ReproducerReplayResult
  ERROR_VARIABLE ReproducerReplayDiagnostics
)
if(NOT ReproducerReplayResult EQUAL 0)
  message(FATAL_ERROR "successful reproducer replay failed: ${ReproducerReplayDiagnostics}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${SuccessAssembly}" "${ReplayedAssembly}"
  RESULT_VARIABLE ReproducerComparisonResult
)
if(NOT ReproducerComparisonResult EQUAL 0)
  message(FATAL_ERROR "reproducer replay changed the artifact bytes")
endif()

set(RegBankInput "${WORK_DIRECTORY}/regbank.mir")
execute_process(
  COMMAND "${TOOL}" --input "${INPUT}" --emit regbank-mir -o "${RegBankInput}"
  RESULT_VARIABLE RegBankResult
  ERROR_VARIABLE RegBankDiagnostics
)
if(NOT RegBankResult EQUAL 0)
  message(FATAL_ERROR "register-bank seed production failed: ${RegBankDiagnostics}")
endif()

set(FailureReproducer "${WORK_DIRECTORY}/failure-reproducer")
execute_process(
  COMMAND
    "${TOOL}" --input "${RegBankInput}" -run-pass instruction-select --emit selected-mir
    --fuel 0 --crash-reproducer "${FailureReproducer}" -o "${WORK_DIRECTORY}/failure.mir"
  RESULT_VARIABLE FailureResult
  ERROR_VARIABLE FailureDiagnostics
)
if(FailureResult EQUAL 0 OR
   NOT FailureDiagnostics MATCHES "required selection action was rejected")
  message(FATAL_ERROR "zero Action fuel did not produce the expected failure")
endif()
file(READ "${FailureReproducer}/manifest.yaml" FailureManifest)
if(NOT FailureManifest MATCHES "instruction-select" OR
   NOT FailureManifest MATCHES "required selection action was rejected")
  message(FATAL_ERROR "failure reproducer did not record structured pass diagnostics")
endif()
execute_process(
  COMMAND "${TOOL}" --reproduce "${FailureReproducer}" -o "${WORK_DIRECTORY}/replay.mir"
  RESULT_VARIABLE FailureReplayResult
  ERROR_VARIABLE FailureReplayDiagnostics
)
if(FailureReplayResult EQUAL 0 OR
   NOT FailureReplayDiagnostics MATCHES "required selection action was rejected")
  message(FATAL_ERROR "failure reproducer did not replay the original failure")
endif()

set(ActionOutput "${WORK_DIRECTORY}/selected.mir")
execute_process(
  COMMAND
    "${TOOL}" --input "${RegBankInput}" -run-pass instruction-select --emit selected-mir
    --print-actions -o "${ActionOutput}"
  RESULT_VARIABLE ActionResult
  ERROR_VARIABLE ActionDiagnostics
)
if(NOT ActionResult EQUAL 0 OR NOT ActionDiagnostics MATCHES "accepted instruction-select")
  message(FATAL_ERROR "successful Action execution did not report stable decisions")
endif()

file(READ "${INPUT}" ReducerSeedText)
string(REPLACE
  "    $x10 = COPY %2\n"
  "    %3:gprb(i64) = G_CONSTANT 99\n    $x10 = COPY %2\n"
  ReducerSeedText
  "${ReducerSeedText}"
)
set(ReducerSeed "${WORK_DIRECTORY}/reducer-seed.mir")
set(CanonicalReducerSeed "${WORK_DIRECTORY}/reducer-seed.canonical.mir")
set(ReducedMIR "${WORK_DIRECTORY}/reduced.mir")
file(WRITE "${ReducerSeed}" "${ReducerSeedText}")
execute_process(
  COMMAND "${TOOL}" --verify --input "${ReducerSeed}" -o "${CanonicalReducerSeed}"
  RESULT_VARIABLE ReducerSeedResult
  ERROR_VARIABLE ReducerSeedDiagnostics
)
if(NOT ReducerSeedResult EQUAL 0)
  message(FATAL_ERROR "reducer seed failed canonicalization: ${ReducerSeedDiagnostics}")
endif()
execute_process(
  COMMAND "${TOOL}" --reduce validity --input "${ReducerSeed}" -o "${ReducedMIR}"
  RESULT_VARIABLE ValidityReducerResult
  ERROR_VARIABLE ValidityReducerDiagnostics
)
if(NOT ValidityReducerResult EQUAL 0)
  message(FATAL_ERROR "validity reduction failed: ${ValidityReducerDiagnostics}")
endif()
file(SIZE "${CanonicalReducerSeed}" CanonicalReducerSeedSize)
file(SIZE "${ReducedMIR}" ReducedMIRSize)
if(NOT ReducedMIRSize LESS CanonicalReducerSeedSize)
  message(FATAL_ERROR "validity reducer did not make the canonical seed smaller")
endif()
execute_process(
  COMMAND "${TOOL}" --verify --input "${ReducedMIR}"
  RESULT_VARIABLE ReducedVerifyResult
  ERROR_VARIABLE ReducedVerifyDiagnostics
)
if(NOT ReducedVerifyResult EQUAL 0)
  message(FATAL_ERROR "validity reducer produced invalid MIR: ${ReducedVerifyDiagnostics}")
endif()

set(FailureReducedMIR "${WORK_DIRECTORY}/failure-reduced.mir")
execute_process(
  COMMAND
    "${TOOL}" --reduce failure --input "${RegBankInput}" -run-pass instruction-select
    --emit selected-mir --fuel 0 -o "${FailureReducedMIR}"
  RESULT_VARIABLE FailureReducerResult
  ERROR_VARIABLE FailureReducerDiagnostics
)
if(NOT FailureReducerResult EQUAL 0)
  message(FATAL_ERROR "failure reduction failed: ${FailureReducerDiagnostics}")
endif()
execute_process(
  COMMAND
    "${TOOL}" --input "${FailureReducedMIR}" -run-pass instruction-select --emit selected-mir
    --fuel 0 -o "${WORK_DIRECTORY}/failure-reduced-output.mir"
  RESULT_VARIABLE ReducedFailureResult
  ERROR_VARIABLE ReducedFailureDiagnostics
)
if(ReducedFailureResult EQUAL 0 OR
   NOT ReducedFailureDiagnostics MATCHES "required selection action was rejected")
  message(FATAL_ERROR "failure reducer did not preserve its interestingness condition")
endif()

set(FirstFuzz "${WORK_DIRECTORY}/fuzz-1.txt")
set(SecondFuzz "${WORK_DIRECTORY}/fuzz-2.txt")
execute_process(
  COMMAND
    "${TOOL}" --fuzz all --fuzz-iterations 2 --seed 123 --input "${INPUT}" -o
    "${FirstFuzz}"
  RESULT_VARIABLE FirstFuzzResult
  ERROR_VARIABLE FirstFuzzDiagnostics
)
if(NOT FirstFuzzResult EQUAL 0)
  message(FATAL_ERROR "layered fuzz execution failed: ${FirstFuzzDiagnostics}")
endif()
execute_process(
  COMMAND
    "${TOOL}" --fuzz all --fuzz-iterations 2 --seed 123 --input "${INPUT}" -o
    "${SecondFuzz}"
  RESULT_VARIABLE SecondFuzzResult
  ERROR_VARIABLE SecondFuzzDiagnostics
)
if(NOT SecondFuzzResult EQUAL 0)
  message(FATAL_ERROR "repeated layered fuzz execution failed: ${SecondFuzzDiagnostics}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files "${FirstFuzz}" "${SecondFuzz}"
  RESULT_VARIABLE FuzzComparisonResult
)
if(NOT FuzzComparisonResult EQUAL 0)
  message(FATAL_ERROR "layered fuzz accounting is not deterministic")
endif()
file(READ "${FirstFuzz}" FuzzText)
if(NOT FuzzText MATCHES "seed=123 executed=11" OR
   NOT FuzzText MATCHES "parser: executed=2" OR
   NOT FuzzText MATCHES "builder: executed=2" OR
   NOT FuzzText MATCHES "property-stage: executed=2" OR
   NOT FuzzText MATCHES "mutation: executed=2" OR
   NOT FuzzText MATCHES "pipeline: executed=2" OR
   NOT FuzzText MATCHES "semantics: executed=1")
  message(FATAL_ERROR "layered fuzz output does not account for all six layers")
endif()
execute_process(
  COMMAND "${TOOL}" --fuzz parser,parser --input "${INPUT}"
  RESULT_VARIABLE DuplicateFuzzResult
  ERROR_VARIABLE DuplicateFuzzDiagnostics
)
if(DuplicateFuzzResult EQUAL 0 OR NOT DuplicateFuzzDiagnostics MATCHES "must be unique")
  message(FATAL_ERROR "CLI fuzzing accepted duplicate layers")
endif()

file(REMOVE_RECURSE "${WORK_DIRECTORY}")
