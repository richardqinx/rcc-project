# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.25)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/Modules/CMakePolicy.cmake" NO_POLICY_SCOPE)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/Modules/RCCVersion.cmake")

project(RCC VERSION "${RCC_VERSION}" LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "RCC requires an out-of-source build.")
endif()

set(RCC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(RCC_MAIN_SRC_DIR "${RCC_SOURCE_DIR}")
set(RCC_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(RCC_CMAKE_DIR "${RCC_SOURCE_DIR}/cmake/modules")
set(RCC_INCLUDE_DIRS "${RCC_SOURCE_DIR}/include;${RCC_BINARY_DIR}/include")
set(RCC_ENABLE_PROJECTS "" CACHE STRING "Additional projects to build: cfront")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export the compilation database")
file(MAKE_DIRECTORY "${RCC_BINARY_DIR}/include/rcc/Config")

list(PREPEND CMAKE_MODULE_PATH "${RCC_CMAKE_DIR}")
include(GNUInstallDirs)
include(HandleRCCOptions)
include(AddRCC)
rcc_configure_options()
rcc_configure_output_directories()
set(RCC_LIBRARY_DIR "${RCC_LIBRARY_OUTPUT_INTDIR}")

set(RCC_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}" CACHE STRING
  "Target architecture used for RCC configuration")
set(RCC_TARGETS_TO_BUILD "Native" CACHE STRING
  "Target architectures built by RCC")
option(RCC_ENABLE_THREADS "Use threads if available." ON)
set(RCC_DEFAULT_TARGET_TRIPLE "" CACHE STRING
  "Target triple RCC will generate code for by default")
include("${RCC_SOURCE_DIR}/cmake/config-ix.cmake")
if(RCC_DEFAULT_TARGET_TRIPLE STREQUAL "")
  set(RCC_DEFAULT_TARGET_TRIPLE "${RCC_HOST_TRIPLE}" CACHE STRING
    "Target triple RCC will generate code for by default" FORCE)
endif()

set(RCC_MAIN_INCLUDE_DIR "${RCC_SOURCE_DIR}/include")
set(RCC_INCLUDE_DIR "${RCC_BINARY_DIR}/include")
set(PACKAGE_VERSION "${RCC_VERSION}")
set(RCC_BUILD_RCC_DYLIB OFF)
set(RCC_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")

configure_file(
  "${RCC_MAIN_INCLUDE_DIR}/rcc/Config/config.h.cmake"
  "${RCC_INCLUDE_DIR}/rcc/Config/config.h")
configure_file(
  "${RCC_MAIN_INCLUDE_DIR}/rcc/Config/rcc-config.h.cmake"
  "${RCC_INCLUDE_DIR}/rcc/Config/rcc-config.h")

add_library(RCCHeaders INTERFACE)
target_compile_features(RCCHeaders INTERFACE cxx_std_23)
target_include_directories(RCCHeaders INTERFACE
  "$<BUILD_INTERFACE:${RCC_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${RCC_BINARY_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
install(TARGETS RCCHeaders EXPORT RCCTargets)
install(DIRECTORY include/rcc DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.h" PATTERN "*.inc" PATTERN "*.def")
install(DIRECTORY "${RCC_BINARY_DIR}/include/rcc" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.h" PATTERN "*.inc" PATTERN "*.def"
  PATTERN "CMakeFiles" EXCLUDE PATTERN "config.h" EXCLUDE)

add_subdirectory(lib)
add_subdirectory(tools)
add_subdirectory(utils)

foreach(rccProject IN LISTS RCC_ENABLE_PROJECTS)
  if(NOT rccProject STREQUAL "cfront")
    message(FATAL_ERROR "Unknown RCC project: ${rccProject}. Available: cfront")
  endif()
endforeach()
if("cfront" IN_LIST RCC_ENABLE_PROJECTS)
  add_subdirectory("${RCC_SOURCE_DIR}/../cfront" "${CMAKE_BINARY_DIR}/cfront")
endif()

add_subdirectory(cmake/modules)
