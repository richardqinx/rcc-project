# SPDX-License-Identifier: GPL-3.0-or-later

function(rcc_backend_add_developer_targets)
  get_property(FormatFiles GLOBAL PROPERTY RCC_BACKEND_FORMAT_FILES)
  get_property(TidyFiles GLOBAL PROPERTY RCC_BACKEND_TIDY_FILES)

  if(FormatFiles)
    list(REMOVE_DUPLICATES FormatFiles)

    add_custom_target(
      rcc-backend-format
      COMMAND "${RCC_BACKEND_CLANG_FORMAT}" -i ${FormatFiles}
      COMMAND_EXPAND_LISTS
      COMMENT "Formatting RCC Backend C++ sources"
      VERBATIM
    )

    add_custom_target(
      rcc-backend-format-check
      COMMAND "${RCC_BACKEND_CLANG_FORMAT}" --dry-run --Werror ${FormatFiles}
      COMMAND_EXPAND_LISTS
      COMMENT "Checking RCC Backend C++ formatting"
      VERBATIM
    )
  endif()

  if(TidyFiles)
    list(REMOVE_DUPLICATES TidyFiles)

    # Match LLVM's normal lint path: pass the changed backend lines to its
    # clang-tidy-diff.py driver, which runs independent translation units in parallel.
    add_custom_target(
      rcc-backend-tidy
      COMMAND
        "${CMAKE_COMMAND}"
        "-DRCC_TIDY_GIT=${RCC_BACKEND_GIT}"
        "-DRCC_TIDY_SOURCE_DIR=${RCC_BACKEND_SOURCE_DIR}"
        "-DRCC_TIDY_BINARY_DIR=${CMAKE_BINARY_DIR}"
        "-DRCC_TIDY_BASE=${RCC_BACKEND_TIDY_BASE}"
        "-DRCC_TIDY_JOBS=${RCC_BACKEND_TIDY_JOBS}"
        "-DRCC_TIDY_DIFF=${RCC_BACKEND_CLANG_TIDY_DIFF}"
        -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/RCCBackendClangTidy.cmake"
      COMMENT "Running incremental clang-tidy on RCC Backend"
      USES_TERMINAL
      VERBATIM
    )

    add_custom_target(
      rcc-backend-tidy-all
      COMMAND
        "${RCC_BACKEND_RUN_CLANG_TIDY}"
        "-p=${CMAKE_BINARY_DIR}"
        -j "${RCC_BACKEND_TIDY_JOBS}"
        -quiet
        "-warnings-as-errors=*"
        "${RCC_BACKEND_SOURCE_DIR}/.*"
      COMMENT "Running full parallel clang-tidy audit on RCC Backend"
      USES_TERMINAL
      VERBATIM
    )

    add_custom_target(
      rcc-backend-analyze
      COMMAND
        "${RCC_BACKEND_RUN_CLANG_TIDY}"
        "-p=${CMAKE_BINARY_DIR}"
        -j "${RCC_BACKEND_TIDY_JOBS}"
        -quiet
        "-checks=-*,clang-analyzer-*"
        "-warnings-as-errors=*"
        "${RCC_BACKEND_SOURCE_DIR}/.*"
      COMMENT "Running full parallel Clang Static Analyzer audit on RCC Backend"
      USES_TERMINAL
      VERBATIM
    )
  endif()
endfunction()
