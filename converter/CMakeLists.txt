# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.28)

project(bethconv
    VERSION 0.0.1
    DESCRIPTION "Ahead-of-time converter from Bethesda game data to open formats"
    # C only because BLAKE3's imported target declares C compile features.
    # We compile no C ourselves.
    LANGUAGES C CXX)

# C++23 for std::expected only. See docs/adr/0001-cxx23.md.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(BETHCONV_BUILD_TESTS "Build the unit test suite" ON)
option(BETHCONV_BUILD_FUZZERS "Build libFuzzer targets (clang only)" OFF)
option(BETHCONV_WERROR "Treat warnings as errors" ON)
set(BETHCONV_SANITIZE "" CACHE STRING "Semicolon-separated sanitizers, e.g. address;undefined")

include(BethconvWarnings)
include(BethconvSanitizers)

add_subdirectory(extern)
add_subdirectory(src)
add_subdirectory(tools)

if(BETHCONV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
