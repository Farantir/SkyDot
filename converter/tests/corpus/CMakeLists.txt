# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests against real game installs. Always built so it keeps compiling; skipped
# at run time when no install is configured, as in CI.
find_package(Catch2 3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
include(Catch)

add_library(bethconv_corpus_probe STATIC corpus_probe.cpp)
target_link_libraries(bethconv_corpus_probe
    PUBLIC bethconv::core
    PRIVATE bethconv::warnings nlohmann_json::nlohmann_json)
target_include_directories(bethconv_corpus_probe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(bethconv_corpus_probe PUBLIC
    BETHCONV_CORPUS_EXPECTATIONS="${CMAKE_CURRENT_SOURCE_DIR}/corpus-expectations.json")

add_executable(bethconv_corpus_tests test_corpus.cpp)
target_link_libraries(bethconv_corpus_tests
    PRIVATE bethconv_corpus_probe bethconv::warnings
            Catch2::Catch2WithMain nlohmann_json::nlohmann_json)

# One ctest entry instead of per-case discovery: with installs present the run
# takes minutes, so it sits behind a single test name.
add_test(NAME corpus COMMAND bethconv_corpus_tests)
set_tests_properties(corpus PROPERTIES LABELS corpus TIMEOUT 1800)

add_executable(bethconv_corpus_snapshot corpus_snapshot.cpp)
target_link_libraries(bethconv_corpus_snapshot
    PRIVATE bethconv_corpus_probe bethconv::warnings nlohmann_json::nlohmann_json)
set_target_properties(bethconv_corpus_snapshot PROPERTIES OUTPUT_NAME bethconv-corpus-snapshot)
