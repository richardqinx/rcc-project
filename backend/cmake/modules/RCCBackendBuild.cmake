# SPDX-License-Identifier: GPL-3.0-or-later

function(rcc_backend_initialize_toolchain)
  string(REGEX MATCH "^[0-9]+" RCCBackendClangMajor "${CMAKE_CXX_COMPILER_VERSION}")

  find_program(
    RCC_BACKEND_CLANG_FORMAT
    NAMES "clang-format-${RCCBackendClangMajor}" clang-format
    REQUIRED
  )
  find_program(
    RCC_BACKEND_CLANG_TIDY
    NAMES "clang-tidy-${RCCBackendClangMajor}" clang-tidy
    REQUIRED
  )
  find_program(
    RCC_BACKEND_CLANG_TIDY_DIFF
    NAMES clang-tidy-diff.py
    HINTS "/usr/lib/llvm-${RCCBackendClangMajor}/share/clang"
    REQUIRED
  )
  find_program(
    RCC_BACKEND_RUN_CLANG_TIDY
    NAMES "run-clang-tidy-${RCCBackendClangMajor}" run-clang-tidy
    REQUIRED
  )
  find_program(RCC_BACKEND_GIT NAMES git REQUIRED)

  foreach(Tool IN ITEMS RCC_BACKEND_CLANG_FORMAT RCC_BACKEND_CLANG_TIDY)
    execute_process(
      COMMAND "${${Tool}}" --version
      OUTPUT_VARIABLE ToolVersionOutput
      ERROR_VARIABLE ToolVersionError
      RESULT_VARIABLE ToolVersionResult
    )
    if(NOT ToolVersionResult EQUAL 0)
      message(
        FATAL_ERROR
          "Unable to query ${Tool}: ${ToolVersionOutput}${ToolVersionError}"
      )
    endif()

    string(REGEX MATCH "version[ \t]+([0-9]+)" ToolVersionMatch "${ToolVersionOutput}")
    if(NOT CMAKE_MATCH_1 STREQUAL RCCBackendClangMajor)
      message(
        FATAL_ERROR
          "${Tool} must have Clang major version ${RCCBackendClangMajor}: "
          "${ToolVersionOutput}"
      )
    endif()
  endforeach()

  set(RCC_BACKEND_CLANG_FORMAT "${RCC_BACKEND_CLANG_FORMAT}" PARENT_SCOPE)
  set(RCC_BACKEND_CLANG_TIDY "${RCC_BACKEND_CLANG_TIDY}" PARENT_SCOPE)
  set(RCC_BACKEND_CLANG_TIDY_DIFF "${RCC_BACKEND_CLANG_TIDY_DIFF}" PARENT_SCOPE)
  set(RCC_BACKEND_RUN_CLANG_TIDY "${RCC_BACKEND_RUN_CLANG_TIDY}" PARENT_SCOPE)
  set(RCC_BACKEND_GIT "${RCC_BACKEND_GIT}" PARENT_SCOPE)
endfunction()

function(rcc_backend_configure_target Target)
  target_compile_features("${Target}" PUBLIC cxx_std_23)
  set_target_properties(
    "${Target}"
    PROPERTIES
      CXX_EXTENSIONS OFF
      CXX_STANDARD 23
      CXX_STANDARD_REQUIRED ON
  )

  target_compile_options(
    "${Target}"
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
      -Wshadow
      -Wnon-virtual-dtor
      -Wold-style-cast
      -Wcast-align
      -Woverloaded-virtual
      -Wnull-dereference
      -Wdouble-promotion
      -Wformat=2
  )

  if(RCC_BACKEND_ENABLE_EH)
    target_compile_options("${Target}" PRIVATE -fexceptions)
  else()
    target_compile_options("${Target}" PRIVATE -fno-exceptions)
  endif()

  if(RCC_BACKEND_ENABLE_RTTI)
    target_compile_options("${Target}" PRIVATE -frtti)
  else()
    target_compile_options("${Target}" PRIVATE -fno-rtti)
  endif()

  if(RCC_BACKEND_WARNINGS_AS_ERRORS)
    target_compile_options("${Target}" PRIVATE -Werror)
  endif()

  if(RCC_BACKEND_ENABLE_CLANG_TIDY)
    set_target_properties(
      "${Target}"
      PROPERTIES
        CXX_CLANG_TIDY
          "${RCC_BACKEND_CLANG_TIDY};--warnings-as-errors=*"
    )
  endif()
endfunction()

function(rcc_backend_register_format_files)
  set_property(GLOBAL APPEND PROPERTY RCC_BACKEND_FORMAT_FILES ${ARGN})
endfunction()
