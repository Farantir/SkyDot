# SPDX-License-Identifier: GPL-3.0-or-later
find_package(CLI11 CONFIG REQUIRED)

# Reuses the fixture builders in tests/support, which need nifly and zlib.
find_package(ZLIB REQUIRED)

add_executable(bethconv_testpack main.cpp)
set_target_properties(bethconv_testpack PROPERTIES OUTPUT_NAME bethconv-testpack)

target_compile_definitions(bethconv_testpack PRIVATE
    BETHCONV_VERSION="${PROJECT_VERSION}")

target_link_libraries(bethconv_testpack
    PRIVATE bethconv::core bethconv::warnings CLI11::CLI11 ZLIB::ZLIB nifly)
