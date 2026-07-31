# SPDX-License-Identifier: GPL-3.0-or-later

set(RCC_KNOWN_PROJECTS backend)
set(RCC_DEFAULT_PROJECTS backend)

set(
  RCC_ENABLE_PROJECTS
  "${RCC_DEFAULT_PROJECTS}"
  CACHE STRING
  "Semicolon-separated RCC projects to build, or 'all'"
)

function(rcc_add_enabled_projects RCCSourceRoot)
  if(RCC_ENABLE_PROJECTS STREQUAL "all")
    set(EnabledProjects ${RCC_KNOWN_PROJECTS})
  else()
    set(EnabledProjects ${RCC_ENABLE_PROJECTS})
  endif()

  if(NOT EnabledProjects)
    message(FATAL_ERROR "RCC_ENABLE_PROJECTS must name at least one project")
  endif()

  list(REMOVE_DUPLICATES EnabledProjects)

  foreach(Project IN LISTS EnabledProjects)
    if(NOT Project IN_LIST RCC_KNOWN_PROJECTS)
      message(
        FATAL_ERROR
          "Unknown RCC project '${Project}'. Known projects: ${RCC_KNOWN_PROJECTS}"
      )
    endif()
  endforeach()

  # Component order is controlled here rather than by the user-provided list.
  # Future inter-project dependency closure will also be resolved here.
  if("backend" IN_LIST EnabledProjects)
    add_subdirectory("${RCCSourceRoot}/backend" "${CMAKE_BINARY_DIR}/backend")
  endif()
endfunction()
