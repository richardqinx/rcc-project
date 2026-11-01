# SPDX-License-Identifier: GPL-3.0-or-later

include(CheckSymbolExists)
include(GNUInstallDirs)

if(NOT DEFINED RCC_ENABLE_THREADS)
  set(RCC_ENABLE_THREADS ON)
endif()

if(NOT DEFINED LIBBASE_ENABLE_THREADS)
  set(LIBBASE_ENABLE_THREADS "${RCC_ENABLE_THREADS}")
endif()
if(NOT DEFINED LIBBASE_BUILD_SHARED_LIBS)
  set(LIBBASE_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
endif()
if(NOT DEFINED LIBBASE_BUILD_STATIC)
  if(BUILD_SHARED_LIBS)
    set(LIBBASE_BUILD_STATIC OFF)
  else()
    set(LIBBASE_BUILD_STATIC ON)
  endif()
endif()
if(NOT DEFINED LIBBASE_BUILD_LIBBASE_DYLIB)
  set(LIBBASE_BUILD_LIBBASE_DYLIB OFF)
endif()

set(LIBBASE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(LIBBASE_BINARY_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")
file(MAKE_DIRECTORY "${LIBBASE_BINARY_INCLUDE_DIR}/core/config")

check_symbol_exists(strerror_r "string.h" HAVE_STRERROR_R)
check_symbol_exists(strerror_s "string.h" HAVE_DECL_STRERROR_S)

configure_file(
  "${LIBBASE_INCLUDE_DIR}/core/config/config.h.cmake"
  "${LIBBASE_BINARY_INCLUDE_DIR}/core/config/config.h")
configure_file(
  "${LIBBASE_INCLUDE_DIR}/core/config/libbase-config.h.cmake"
  "${LIBBASE_BINARY_INCLUDE_DIR}/core/config/libbase-config.h")

add_library(RCCCore
  src/core/math/MathExtras.cc
  src/core/memory/MemAlloc.cpp
  src/core/error/Errno.cc
)

target_compile_features(RCCCore PUBLIC cxx_std_23)
target_compile_definitions(RCCCore PRIVATE LIBBASE_EXPORTS)

target_include_directories(RCCCore
  PUBLIC
    "$<BUILD_INTERFACE:${LIBBASE_BINARY_INCLUDE_DIR}>"
    "$<BUILD_INTERFACE:${LIBBASE_INCLUDE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

install(TARGETS RCCCore
  EXPORT RCCTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

install(DIRECTORY include/core DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.h" PATTERN "*.inc" PATTERN "*.def")
install(DIRECTORY "${LIBBASE_BINARY_INCLUDE_DIR}/core"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.h" PATTERN "*.inc" PATTERN "*.def"
  PATTERN "config.h" EXCLUDE)
