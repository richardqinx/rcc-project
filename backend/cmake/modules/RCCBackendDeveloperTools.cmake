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

    # LLVM's repository runs clang-tidy-diff.py for changed lines. Keep the
    # backend's explicit full audit deterministic by invoking clang-tidy once
    # with the registered translation units instead of using a process pool.
    add_custom_target(
      rcc-backend-tidy
      COMMAND
        "${RCC_BACKEND_CLANG_TIDY}"
        "--quiet"
        "-p=${CMAKE_BINARY_DIR}"
        "--warnings-as-errors=*"
        ${TidyFiles}
      COMMAND_EXPAND_LISTS
      COMMENT "Running clang-tidy on RCC Backend"
      USES_TERMINAL
      VERBATIM
    )

    add_custom_target(
      rcc-backend-analyze
      COMMAND
        "${RCC_BACKEND_CLANG_TIDY}"
        "--quiet"
        "-p=${CMAKE_BINARY_DIR}"
        "-checks=-*,clang-analyzer-*"
        "--warnings-as-errors=*"
        ${TidyFiles}
      COMMAND_EXPAND_LISTS
      COMMENT "Running Clang Static Analyzer checks on RCC Backend"
      USES_TERMINAL
      VERBATIM
    )
  endif()
endfunction()
