# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.25)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/Modules/CMakePolicy.cmake" NO_POLICY_SCOPE)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/Modules/RCCVersion.cmake")

project(CFront VERSION "${RCC_VERSION}" LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "CFront requires an out-of-source build.")
endif()

if(NOT TARGET RCCHeaders)
  set(CFRONT_STANDALONE_BUILD ON)
  find_package(RCC ${RCC_VERSION} EXACT CONFIG REQUIRED HINTS "${RCC_CMAKE_DIR}")
else()
  set(CFRONT_STANDALONE_BUILD OFF)
endif()
list(PREPEND CMAKE_MODULE_PATH
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules" "${RCC_CMAKE_DIR}")
include(GNUInstallDirs)
include(HandleRCCOptions)
include(AddCFront)
if(CFRONT_STANDALONE_BUILD)
  add_compile_definitions(${RCC_DEFINITIONS})
  foreach(cfrontDefinition IN LISTS RCC_DEFINITIONS)
    list(APPEND CMAKE_REQUIRED_DEFINITIONS "-D${cfrontDefinition}")
  endforeach()
  rcc_configure_options()
  rcc_configure_output_directories()
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export the compilation database")

set(CFRONT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(CFRONT_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
file(MAKE_DIRECTORY "${CFRONT_BINARY_DIR}/include/cfront")
add_library(CFrontHeaders INTERFACE)
target_compile_features(CFrontHeaders INTERFACE cxx_std_23)
target_include_directories(CFrontHeaders INTERFACE
  "$<BUILD_INTERFACE:${CFRONT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CFRONT_BINARY_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(CFrontHeaders INTERFACE RCCHeaders)
install(TARGETS CFrontHeaders EXPORT CFrontTargets)
install(DIRECTORY include/cfront DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.h" PATTERN "*.inc" PATTERN "*.def")
install(DIRECTORY "${CFRONT_BINARY_DIR}/include/cfront" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.h" PATTERN "*.inc" PATTERN "*.def"
  PATTERN "CMakeFiles" EXCLUDE PATTERN "config.h" EXCLUDE)

add_subdirectory(lib)
add_subdirectory(tools)
add_subdirectory(cmake/modules)
