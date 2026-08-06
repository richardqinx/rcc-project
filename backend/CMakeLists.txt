# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.25)

set(RCC_BACKEND_VERSION 0.1.0)
set(RCC_BACKEND_IS_STANDALONE OFF)
set(RCC_BACKEND_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(RCC_BACKEND_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(
    RCCBackend
    VERSION ${RCC_BACKEND_VERSION}
    DESCRIPTION "Independent RCC machine-code backend"
    LANGUAGES CXX
  )
  set(RCC_BACKEND_IS_STANDALONE ON)
  include(CTest)
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
  message(
    FATAL_ERROR
      "RCC Backend must be compiled with Clang; found '${CMAKE_CXX_COMPILER_ID}'"
  )
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(
    FATAL_ERROR
      "RCC Backend does not support in-source builds. Configure a separate build directory."
  )
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(RCCBackendBuild)
include(RCCBackendDeveloperTools)

option(RCC_BACKEND_BUILD_TESTS "Build RCC Backend tests" "${BUILD_TESTING}")
option(RCC_BACKEND_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(RCC_BACKEND_ENABLE_CLANG_TIDY "Run clang-tidy during each compilation" OFF)
option(RCC_BACKEND_ENABLE_EH "Enable C++ exception handling" OFF)
option(RCC_BACKEND_ENABLE_RTTI "Enable C++ run-time type information" OFF)

if(RCC_BACKEND_ENABLE_EH AND NOT RCC_BACKEND_ENABLE_RTTI)
  message(FATAL_ERROR "RCC Backend exception handling requires RTTI")
endif()

rcc_backend_initialize_toolchain()

add_subdirectory(lib)
add_subdirectory(tools)
add_subdirectory(lib/Target)

if(RCC_BACKEND_BUILD_TESTS)
  enable_testing()
  add_subdirectory(unittests)
endif()

rcc_backend_add_developer_targets()

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RCCBackendConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/RCCBackendConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RCCBackend"
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/RCCBackendConfigVersion.cmake"
  VERSION "${RCC_BACKEND_VERSION}"
  COMPATIBILITY SameMajorVersion
)

install(
  EXPORT RCCBackendTargets
  FILE RCCBackendTargets.cmake
  NAMESPACE RCCBackend::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RCCBackend"
)

install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/RCCBackendConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/RCCBackendConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RCCBackend"
)
