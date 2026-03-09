cmake_minimum_required(VERSION 3.24.2)

project(RayTracer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD_REQUIRED True)

if(${CMAKE_GENERATOR} STREQUAL "Unix Makefiles")
  set(CMAKE_MAKE_PROGRAM "${PROJECT_SOURCE_DIR}/scripts/make-parallel.sh" CACHE STRING "" FORCE)
endif()

if(${PROJECT_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
  message(FATAL_ERROR "RayTracer must be built outside its source directory, e.g. `cmake -B build`.")
endif()

# Default is set to Release if not specified
include(${PROJECT_SOURCE_DIR}/etc/build_type.cmake)

# Enable LTO 
include(${PROJECT_SOURCE_DIR}/etc/lto.cmake)

# Enable ccache
include(${PROJECT_SOURCE_DIR}/etc/ccache.cmake)


# strict compile warnings
include(${PROJECT_SOURCE_DIR}/etc/cflags.cmake)

#  clang-format clang-tidy

include(${PROJECT_SOURCE_DIR}/etc/scanners.cmake)

# ctest
# include(${PROJECT_SOURCE_DIR}/etc/ctest.cmake)

# License
install(FILES LICENSE DESTINATION .)

add_subdirectory("${PROJECT_SOURCE_DIR}/src")
add_subdirectory("${PROJECT_SOURCE_DIR}/apps")