# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.25)

project(
  RCC
  VERSION 0.1.0
  DESCRIPTION "RCC compiler collection"
  LANGUAGES CXX
)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

include(RCCPreventInSourceBuild)
include(RCCProjects)
include(CTest)

rcc_add_enabled_projects("${CMAKE_CURRENT_SOURCE_DIR}")

if(TARGET rcc-backend-format)
  add_custom_target(format DEPENDS rcc-backend-format)
endif()

if(TARGET rcc-backend-format-check)
  add_custom_target(format-check DEPENDS rcc-backend-format-check)
endif()

if(TARGET rcc-backend-tidy)
  add_custom_target(tidy DEPENDS rcc-backend-tidy)
endif()

if(TARGET rcc-backend-analyze)
  add_custom_target(analyze DEPENDS rcc-backend-analyze)
endif()
