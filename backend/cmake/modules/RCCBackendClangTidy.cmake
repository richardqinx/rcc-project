# SPDX-License-Identifier: GPL-3.0-or-later

foreach(Variable IN ITEMS RCC_TIDY_GIT RCC_TIDY_SOURCE_DIR RCC_TIDY_BINARY_DIR RCC_TIDY_BASE
                          RCC_TIDY_JOBS RCC_TIDY_DIFF)
  if(NOT DEFINED ${Variable} OR "${${Variable}}" STREQUAL "")
    message(FATAL_ERROR "${Variable} is required for incremental clang-tidy")
  endif()
endforeach()

execute_process(
  COMMAND "${RCC_TIDY_GIT}" -C "${RCC_TIDY_SOURCE_DIR}" rev-parse --show-toplevel
  RESULT_VARIABLE GitRootResult
  OUTPUT_VARIABLE GitRoot
  ERROR_VARIABLE GitRootError
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT GitRootResult EQUAL 0)
  message(FATAL_ERROR "cannot locate the Git worktree: ${GitRootError}")
endif()

file(RELATIVE_PATH TidyPath "${GitRoot}" "${RCC_TIDY_SOURCE_DIR}")
if(TidyPath MATCHES "^\\.\\.")
  message(FATAL_ERROR "backend source directory is outside its Git worktree")
endif()

set(DiffFile "${RCC_TIDY_BINARY_DIR}/CMakeFiles/rcc-backend-clang-tidy.diff")
execute_process(
  COMMAND "${RCC_TIDY_GIT}" -C "${GitRoot}" diff --no-ext-diff --unified=0 --merge-base
          "${RCC_TIDY_BASE}" -- "${TidyPath}"
  RESULT_VARIABLE DiffResult
  OUTPUT_FILE "${DiffFile}"
  ERROR_VARIABLE DiffError
)
if(NOT DiffResult EQUAL 0)
  message(FATAL_ERROR "cannot produce incremental clang-tidy diff: ${DiffError}")
endif()

# git diff does not include untracked source files. Add their ordinary Git diff form so
# LLVM's unchanged clang-tidy-diff.py driver checks new translation units before staging.
execute_process(
  COMMAND "${RCC_TIDY_GIT}" -C "${GitRoot}" ls-files --others --exclude-standard -- "${TidyPath}"
  RESULT_VARIABLE UntrackedResult
  OUTPUT_VARIABLE UntrackedFiles
  ERROR_VARIABLE UntrackedError
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT UntrackedResult EQUAL 0)
  message(FATAL_ERROR "cannot list untracked clang-tidy files: ${UntrackedError}")
endif()
if(NOT UntrackedFiles STREQUAL "")
  string(REPLACE "\n" ";" UntrackedFiles "${UntrackedFiles}")
  foreach(UntrackedFile IN LISTS UntrackedFiles)
    execute_process(
      COMMAND "${RCC_TIDY_GIT}" -C "${GitRoot}" diff --no-index --no-ext-diff --unified=0 --
              /dev/null "${UntrackedFile}"
      RESULT_VARIABLE UntrackedDiffResult
      OUTPUT_VARIABLE UntrackedDiff
      ERROR_VARIABLE UntrackedDiffError
    )
    if(NOT UntrackedDiffResult EQUAL 0 AND NOT UntrackedDiffResult EQUAL 1)
      message(FATAL_ERROR "cannot diff untracked file '${UntrackedFile}': ${UntrackedDiffError}")
    endif()
    file(APPEND "${DiffFile}" "${UntrackedDiff}")
  endforeach()
endif()

execute_process(
  COMMAND "${RCC_TIDY_DIFF}" -path "${RCC_TIDY_BINARY_DIR}" -p1 -quiet -j "${RCC_TIDY_JOBS}"
  INPUT_FILE "${DiffFile}"
  WORKING_DIRECTORY "${GitRoot}"
  RESULT_VARIABLE TidyResult
  OUTPUT_VARIABLE TidyOutput
  ERROR_VARIABLE TidyError
)
set(TidyDiagnostics "${TidyOutput}\n${TidyError}")
string(REGEX REPLACE "[0-9]+ warnings generated\\." "" TidyDiagnostics "${TidyDiagnostics}")
string(STRIP "${TidyDiagnostics}" TidyDiagnostics)
if(NOT TidyDiagnostics STREQUAL "")
  message("${TidyDiagnostics}")
endif()
if(NOT TidyResult EQUAL 0 OR NOT TidyDiagnostics STREQUAL "")
  message(FATAL_ERROR "incremental clang-tidy failed")
endif()
