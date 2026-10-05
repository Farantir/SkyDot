# SPDX-License-Identifier: GPL-3.0-or-later
#
# Each target is built up to twice:
#
#   * `bethconv-fuzz-<name>` links libFuzzer. Needs clang; built only with
#     BETHCONV_BUILD_FUZZERS, and a configure error if the toolchain lacks it.
#   * `bethconv-fuzz-<name>-replay` links a plain main() that replays files.
#     Built with any compiler so the targets keep compiling and ctest runs them.
find_package(ZLIB REQUIRED)
find_package(bsa CONFIG REQUIRED)

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
check_cxx_source_compiles([[
extern "C" int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long) { return 0; }
]] BETHCONV_HAVE_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)

if(BETHCONV_BUILD_FUZZERS AND NOT BETHCONV_HAVE_LIBFUZZER)
    message(FATAL_ERROR
        "BETHCONV_BUILD_FUZZERS=ON but this toolchain has no libFuzzer "
        "(-fsanitize=fuzzer). Use the linux-fuzz preset, which selects clang.")
endif()

set(BETHCONV_FUZZ_TARGETS esm forms bsa nif dds strings pex lod assets animation)

foreach(name IN LISTS BETHCONV_FUZZ_TARGETS)
    # The replay build: always available.
    add_executable(bethconv_fuzz_${name}_replay fuzz_${name}.cpp fuzz_main.cpp)
    target_link_libraries(bethconv_fuzz_${name}_replay
        PRIVATE bethconv::core bethconv::warnings)
    set_target_properties(bethconv_fuzz_${name}_replay PROPERTIES
        OUTPUT_NAME bethconv-fuzz-${name}-replay)

    if(BETHCONV_BUILD_FUZZERS)
        add_executable(bethconv_fuzz_${name} fuzz_${name}.cpp)
        target_link_libraries(bethconv_fuzz_${name}
            PRIVATE bethconv::core bethconv::warnings)
        target_compile_options(bethconv_fuzz_${name} PRIVATE -fsanitize=fuzzer)
        target_link_options(bethconv_fuzz_${name} PRIVATE -fsanitize=fuzzer)
        set_target_properties(bethconv_fuzz_${name} PROPERTIES
            OUTPUT_NAME bethconv-fuzz-${name})
    endif()
endforeach()

# Seed generator. Uses the same synthetic writers as the unit tests (rsm-bsa,
# nifly, zlib), so the corpus is generated rather than committed.
add_executable(bethconv_fuzz_seeds fuzz_seeds.cpp)
target_link_libraries(bethconv_fuzz_seeds
    PRIVATE bethconv::core bethconv::warnings ZLIB::ZLIB bsa::bsa nifly)
target_include_directories(bethconv_fuzz_seeds PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
set_target_properties(bethconv_fuzz_seeds PROPERTIES OUTPUT_NAME bethconv-fuzz-seeds)

# Generates the seeds and replays them through every target. A smoke test, not
# fuzzing.
add_test(NAME fuzz-replay
    COMMAND ${CMAKE_COMMAND}
        -DSEEDS_TOOL=$<TARGET_FILE:bethconv_fuzz_seeds>
        -DSEEDS_DIR=${CMAKE_CURRENT_BINARY_DIR}/seeds
        -DBIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/replay.cmake)
set_tests_properties(fuzz-replay PROPERTIES LABELS fuzz TIMEOUT 600)
