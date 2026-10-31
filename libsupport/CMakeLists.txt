# SPDX-License-Identifier: GPL-3.0-or-later

add_rcc_library(RCCSupport
  STATIC
  src/corelib/math/MathExtras.cc
  src/corelib/memory/MemAlloc.cpp
  src/corelib/system/Errno.cc
)

target_include_directories(RCCSupport
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

install(DIRECTORY include/CoreLib DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
