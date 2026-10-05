# SPDX-License-Identifier: GPL-3.0-or-later
find_package(CLI11 CONFIG REQUIRED)

add_executable(bethconv_cli
    main.cpp
    common.cpp
    front_end.cpp
    commands/animation.cpp
    commands/cell.cpp
    commands/convert.cpp
    commands/detect.cpp
    commands/extract.cpp
    commands/forms.cpp
    commands/info.cpp
    commands/loadorder.cpp
    commands/merge.cpp
    commands/mesh.cpp
    commands/mo2.cpp
    commands/probe.cpp
    commands/records.cpp
    commands/scan.cpp
    commands/script.cpp
    commands/strings.cpp
    commands/target.cpp
    commands/texture.cpp
    commands/view.cpp)
find_package(nlohmann_json CONFIG REQUIRED)
set_target_properties(bethconv_cli PROPERTIES OUTPUT_NAME bethconv)

# Sources include "common.hpp", "front_end.hpp" and "commands/commands.hpp".
target_include_directories(bethconv_cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(bethconv_cli PRIVATE
    BETHCONV_VERSION="${PROJECT_VERSION}")

target_link_libraries(bethconv_cli
    PRIVATE bethconv::core bethconv::warnings CLI11::CLI11 nlohmann_json::nlohmann_json)
