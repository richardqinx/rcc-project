# SPDX-License-Identifier: GPL-3.0-or-later

# Resolve developer tools only when the corresponding target is invoked.
set(RCC_PYTHON_EXECUTABLE python3 CACHE STRING "Python interpreter for developer tools")
set(RCC_CLANG_FORMAT_EXECUTABLE clang-format CACHE STRING "clang-format executable")
set(RCC_CLANG_TIDY_EXECUTABLE clang-tidy CACHE STRING "clang-tidy executable")
set(RCC_ANALYZER_EXECUTABLE clang++ CACHE STRING "Clang static analyzer executable")
set(RCC_CLANG_FORMAT_VERSION "" CACHE STRING "Required clang-format version, if specified")
set(RCC_CLANG_TIDY_VERSION "" CACHE STRING "Required clang-tidy version, if specified")
set(RCC_ANALYZER_VERSION "" CACHE STRING "Required Clang analyzer version, if specified")
set(RCC_DEV_BASE HEAD CACHE STRING "Git revision used for changed-file checks")

set(rccDevScript "${CMAKE_CURRENT_SOURCE_DIR}/dev.py")
add_custom_target(format-check
  COMMAND "${RCC_PYTHON_EXECUTABLE}" "${rccDevScript}" format
    --build-dir "${CMAKE_BINARY_DIR}"
    --base "${RCC_DEV_BASE}"
    --tool "${RCC_CLANG_FORMAT_EXECUTABLE}"
    --tool-version "${RCC_CLANG_FORMAT_VERSION}"
  WORKING_DIRECTORY "${RCC_SOURCE_DIR}/.."
  USES_TERMINAL VERBATIM)
add_custom_target(format
  COMMAND "${RCC_PYTHON_EXECUTABLE}" "${rccDevScript}" format --fix
    --build-dir "${CMAKE_BINARY_DIR}"
    --base "${RCC_DEV_BASE}"
    --tool "${RCC_CLANG_FORMAT_EXECUTABLE}"
    --tool-version "${RCC_CLANG_FORMAT_VERSION}"
  WORKING_DIRECTORY "${RCC_SOURCE_DIR}/.."
  USES_TERMINAL VERBATIM)
add_custom_target(tidy
  COMMAND "${RCC_PYTHON_EXECUTABLE}" "${rccDevScript}" tidy
    --build-dir "${CMAKE_BINARY_DIR}" --base "${RCC_DEV_BASE}"
    --tool "${RCC_CLANG_TIDY_EXECUTABLE}"
    --tool-version "${RCC_CLANG_TIDY_VERSION}"
  WORKING_DIRECTORY "${RCC_SOURCE_DIR}/.."
  USES_TERMINAL VERBATIM)
add_custom_target(analyze
  COMMAND "${RCC_PYTHON_EXECUTABLE}" "${rccDevScript}" analyze
    --build-dir "${CMAKE_BINARY_DIR}" --base "${RCC_DEV_BASE}"
    --tool "${RCC_ANALYZER_EXECUTABLE}"
    --tool-version "${RCC_ANALYZER_VERSION}"
  WORKING_DIRECTORY "${RCC_SOURCE_DIR}/.."
  USES_TERMINAL VERBATIM)
