find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSESW REQUIRED IMPORTED_TARGET ncursesw)
find_package(Threads REQUIRED)
# ICU 76.1 supplies UTF-8 grapheme/line boundaries and Unicode properties.
pkg_check_modules(ICU REQUIRED IMPORTED_TARGET icu-uc>=76.1 icu-i18n>=76.1)

add_library(ricochet_tui STATIC document_text.cc document.cc mailbox.cc screen.cc session.cc text.cc layout.cc)
target_include_directories(ricochet_tui PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(ricochet_tui PUBLIC cxx_std_23)
target_link_libraries(ricochet_tui PRIVATE PkgConfig::NCURSESW PkgConfig::ICU Threads::Threads)
