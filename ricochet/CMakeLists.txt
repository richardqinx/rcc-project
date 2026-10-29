cmake_minimum_required(VERSION 3.24.2)

project(
    Ricochet
    VERSION 0.1.0
    DESCRIPTION "A minimalist CLI web browser for fast, distraction-free navigation."
    LANGUAGES CXX
)

if(${PROJECT_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
  message(FATAL_ERROR "Ricochet must be built outside its source directory, e.g. `cmake -B build`.")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/lib)
endforeach()

include(etc/build_type.cmake)
include(etc/cflags.cmake)
include(etc/scanners.cmake)
include(etc/tests.cmake)

add_subdirectory(src)
add_subdirectory(tests)

install(TARGETS ricochet
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
)

if(NOT TARGET uninstall)
  configure_file(
    "${PROJECT_SOURCE_DIR}/etc/cmake_uninstall.cmake.in"
    "${PROJECT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

  add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P "${PROJECT_BINARY_DIR}/cmake_uninstall.cmake")
endif()