file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS "*.cc")

set(MAIN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/main.cc")
list(REMOVE_ITEM ALL_SOURCES ${MAIN_SRC})

add_library(ricochet_core STATIC ${ALL_SOURCES})
target_include_directories(ricochet_core PUBLIC ${PROJECT_SOURCE_DIR}/include)

add_executable(ricochet ${MAIN_SRC})
target_link_libraries(ricochet PRIVATE ricochet_core)
# target_link_libraries(ricochet PRIVATE ricochet_core curl ncurses)