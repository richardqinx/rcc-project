# SPDX-License-Identifier: GPL-3.0-or-later

include_guard(GLOBAL)
include(AddRCC)

function(add_cfront_library name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "DISABLE_PCH_REUSE;NO_INSTALL_RPATH" "" "LINK_LIBS;PRECOMPILE_HEADERS")
  add_library(${name} ${arg_UNPARSED_ARGUMENTS})
  target_link_libraries(${name} PUBLIC CFrontHeaders ${arg_LINK_LIBS})
  rcc_configure_target(${name})
  install(TARGETS ${name} EXPORT CFrontTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endfunction()

function(add_cfront_tool name)
  add_rcc_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE CFrontHeaders)
  install(TARGETS ${name} EXPORT CFrontTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endfunction()
