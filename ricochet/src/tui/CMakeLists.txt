find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSESW REQUIRED IMPORTED_TARGET ncursesw)
find_package(Threads REQUIRED)

add_library(ricochet_tui STATIC document.cc mailbox.cc screen.cc session.cc)
target_include_directories(ricochet_tui PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(ricochet_tui PUBLIC cxx_std_23)
target_link_libraries(ricochet_tui PRIVATE PkgConfig::NCURSESW Threads::Threads)
