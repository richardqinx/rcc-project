include(CTest)

# list(APPEND CMAKE_CTEST_ARGUMENTS --output-on-failure --stop-on-failure --timeout 15 -E 'speed_test|optimization|webget')

# set(compile_name "compile with bug-checkers")
# add_test(NAME ${compile_name}
#   COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" -t functionality_testing webget)

# macro (ttest name)
#   add_test(NAME ${name} COMMAND "${name}_sanitized")
#   set_property(TEST ${name} PROPERTY FIXTURES_REQUIRED compile)
# endmacro (ttest)

# set_property(TEST ${compile_name} PROPERTY TIMEOUT 0)
# set_tests_properties(${compile_name} PROPERTIES FIXTURES_SETUP compile)

# add_test(NAME t_webget COMMAND "${PROJECT_SOURCE_DIR}/tests/webget_t.sh" "${PROJECT_BINARY_DIR}")
# set_property(TEST t_webget PROPERTY FIXTURES_REQUIRED compile)

