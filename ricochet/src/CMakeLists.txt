add_subdirectory(tui)

file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    "core/*.cc"
    "net/*.cc"
    "parser/*.cc"
    "render/*.cc"
)

set(MAIN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/main.cc")

find_package(OpenSSL REQUIRED)
message(STATUS "Found OpenSSL Version: ${OPENSSL_VERSION}")
message(STATUS "OpenSSL Include dir: ${OPENSSL_INCLUDE_DIR}")

add_library(ricochet_core STATIC ${CORE_SOURCES})
target_include_directories(ricochet_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(ricochet_core 
    PUBLIC 
        OpenSSL::SSL 
        OpenSSL::Crypto
)

add_executable(ricochet ${MAIN_SRC})
target_link_libraries(ricochet PRIVATE ricochet_core)
