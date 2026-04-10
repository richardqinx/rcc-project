file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS "*.cc")

set(MAIN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/main.cc")
list(REMOVE_ITEM ALL_SOURCES ${MAIN_SRC})

find_package(OpenSSL REQUIRED)
message(STATUS "Found OpenSSL Version: ${OPENSSL_VERSION}")
message(STATUS "OpenSSL Include dir: ${OPENSSL_INCLUDE_DIR}")

add_library(ricochet_core STATIC ${ALL_SOURCES})
target_include_directories(ricochet_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(ricochet_core 
    PUBLIC 
        OpenSSL::SSL 
        OpenSSL::Crypto
)

add_executable(ricochet ${MAIN_SRC})
target_link_libraries(ricochet PRIVATE ricochet_core)
# target_link_libraries(ricochet PRIVATE ricochet_core curl ncurses)