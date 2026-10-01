# SPDX-License-Identifier: GPL-3.0-or-later
find_package(CLI11 CONFIG REQUIRED)

add_executable(bethconv_cli main.cpp)
set_target_properties(bethconv_cli PROPERTIES OUTPUT_NAME bethconv)

target_compile_definitions(bethconv_cli PRIVATE
    BETHCONV_VERSION="${PROJECT_VERSION}")

target_link_libraries(bethconv_cli
    PRIVATE bethconv::core bethconv::warnings CLI11::CLI11)
