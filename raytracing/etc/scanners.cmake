# file (GLOB_RECURSE RayTracer_CC_FILES ${CMAKE_SOURCE_DIR}/src/*.cc)

# file (GLOB_RECURSE ALL_SRC_FILES *.hh *.cc)

# add_custom_target (format "clang-format" -i ${ALL_SRC_FILES} COMMENT "Formatting source code...")


find_program(CLANG_FORMAT_EXE clang-format)
if(CLANG_FORMAT_EXE)
    file(GLOB_RECURSE ALL_FMT_FILES
        "${CMAKE_SOURCE_DIR}/src/*.cc"
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/*.h"
        "${CMAKE_SOURCE_DIR}/src/*.hpp"
        "${CMAKE_SOURCE_DIR}/apps/*.cc"
        "${CMAKE_SOURCE_DIR}/apps/*.cpp"
    )
    
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXE} -i -style=file ${ALL_FMT_FILES}
        COMMENT "Formatting source code..."
    )
else()
    message(WARNING "clang-format not found! 'format' target will be disabled.")
endif()

# clang-tidy
# find_program(CLANG_TIDY_EXE clang-tidy)
# if(CLANG_TIDY_EXE)
#     message(STATUS "Clang-Tidy enabled: will run automatically during compilation.")
#     set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
# else()
#     message(WARNING "clang-tidy not found! Static analysis will be disabled.")
# endif()

foreach (tidy_target ${ALL_SRC_FILES})
  get_filename_component (basename ${tidy_target} NAME)
  get_filename_component (dirname ${tidy_target} DIRECTORY)
  get_filename_component (basedir ${dirname} NAME)
  set (tidy_target_name "${basedir}__${basename}")
  set (tidy_command clang-tidy --quiet -header-filter=.* -p=${PROJECT_BINARY_DIR} ${tidy_target})
  add_custom_target (tidy_${tidy_target_name} ${tidy_command})
  list (APPEND ALL_TIDY_TARGETS tidy_${tidy_target_name})

  if (${tidy_target} IN_LIST RayTracer_CC_FILES)
    list (APPEND RayTracer_TIDY_TARGETS tidy_${tidy_target_name})
  endif ()
endforeach (tidy_target)

add_custom_target (tidy DEPENDS ${RayTracer_TIDY_TARGETS})

add_custom_target (tidy-all DEPENDS ${ALL_TIDY_TARGETS})
