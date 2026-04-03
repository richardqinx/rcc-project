# ─── GoogleTest ───────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# Prevent GoogleTest from overriding our compiler/linker options
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# ─── Helper: define a test target ────────────────────────────────────────────
function(atp_add_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME} PRIVATE atp_core GTest::gtest_main)
    gtest_discover_tests(${TEST_NAME})
endfunction()

# ─── Test Targets ────────────────────────────────────────────────────────────
atp_add_test(test_term_bank       test_term_bank.cc)
atp_add_test(test_unification     test_unification.cc)
atp_add_test(test_clausifier      test_clausifier.cc)
atp_add_test(test_resolution      test_resolution.cc)
atp_add_test(test_discrim_tree    test_discrim_tree.cc)
atp_add_test(test_prover          test_prover.cc)
atp_add_test(test_simplify        test_simplify.cc)
atp_add_test(test_proof_trace     test_proof_trace.cc)
atp_add_test(test_end_to_end      test_end_to_end.cc)
