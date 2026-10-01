# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.28)

project(skydot
    VERSION 0.0.1
    DESCRIPTION "A Godot engine for packs written by bethconv"
    LANGUAGES CXX)

# C++20. bethconv needs C++23 for std::expected; errors here cross the
# boundary as godot::Error instead.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(SKYDOT_BUILD_TESTS "Register the headless Godot smoke tests with ctest" ON)
option(SKYDOT_WERROR "Treat warnings as errors" ON)

# godot-cpp options; set before add_subdirectory, since godotcpp_options()
# reads them as cache variables.
#
# The API version selects which extension_api JSON godot-cpp generates from and
# must match compatibility_minimum in the .gdextension file. Newer editors load
# extensions built for older APIs; older editors refuse newer ones with a
# message.
set(GODOTCPP_API_VERSION "4.7" CACHE STRING "Godot API the bindings are generated for")
set(GODOTCPP_TARGET "template_debug" CACHE STRING "godot-cpp target: template_debug or template_release")
set(GODOTCPP_SYSTEM_HEADERS ON CACHE BOOL "godot-cpp headers are SYSTEM so our warning set stays ours")
set(GODOTCPP_USE_HOT_RELOAD "" CACHE BOOL "Hot reload accounting; the debug presets turn it on")

include(SkydotWarnings)

add_subdirectory(extern/godot-cpp)

# FlatBuffers: headers for reading, flatc to generate the world.fb reader.
set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATLIB OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATHASH OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_INSTALL OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATC ON CACHE BOOL "" FORCE)
add_subdirectory(extern/flatbuffers EXCLUDE_FROM_ALL)
add_subdirectory(extension)

if(SKYDOT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
