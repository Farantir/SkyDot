# SPDX-License-Identifier: GPL-3.0-or-later
#
# The GDExtension: one shared library named like godot-cpp's own builds
# (`libskydot.linux.template_debug.x86_64.so`), so game/skydot.gdextension can
# list every platform and configuration.
# world.fb, script and LOD asset readers, generated from copies of bethconv's
# schemas. The copies are compared with bethconv's by the schema_matches tests.
set(SKYDOT_FB_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SKYDOT_WORLD_HEADER "${SKYDOT_FB_DIR}/world_generated.h")
set(SKYDOT_SCRIPT_HEADER "${SKYDOT_FB_DIR}/script_generated.h")
set(SKYDOT_LOD_HEADER "${SKYDOT_FB_DIR}/lod_generated.h")
set(SKYDOT_ANIMATION_HEADER "${SKYDOT_FB_DIR}/animation_generated.h")
foreach(schema IN ITEMS world script lod animation)
    add_custom_command(
        OUTPUT "${SKYDOT_FB_DIR}/${schema}_generated.h"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SKYDOT_FB_DIR}"
        COMMAND flatc --cpp --cpp-std c++17 --scoped-enums -o "${SKYDOT_FB_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../../formats/schema/${schema}.fbs"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../formats/schema/${schema}.fbs" flatc
        COMMENT "flatc: ${schema}.fbs -> ${schema}_generated.h"
        VERBATIM)
endforeach()

add_library(skydot SHARED)

target_sources(skydot PRIVATE
    src/register_types.cpp
    src/register_types.hpp
    src/assets/asset_cache.cpp
    src/assets/asset_cache.hpp
    src/assets/model.cpp
    src/assets/model.hpp
    src/assets/pack.cpp
    src/assets/pack.hpp
    src/assets/pack_store.cpp
    src/assets/pack_store.hpp
    src/world/actor_animation.cpp
    src/world/actors.cpp
    src/world/actors.hpp
    src/world/actor_animation.hpp
    src/world/animator.cpp
    src/world/animator.hpp
    src/world/billboard.cpp
    src/world/billboard.hpp
    src/world/collision.cpp
    src/world/navmesh.cpp
    src/world/weather.cpp
    src/world/collision.hpp
    src/world/effect_asset.cpp
    src/world/effect_asset.hpp
    src/world/flicker.cpp
    src/world/flicker.hpp
    src/world/water.cpp
    src/world/water.hpp
    src/world/terrain.cpp
    src/world/terrain.hpp
    src/world/materials.cpp
    src/world/materials.hpp
    src/world/particles.cpp
    src/world/particles.hpp
    src/world/player.cpp
    src/world/player.hpp
    src/vm/papyrus.cpp
    src/vm/papyrus.hpp
    src/vm/quests.cpp
    src/vm/save.cpp
    src/vm/script_class.cpp
    src/vm/script_class.hpp
    src/vm/value.hpp
    src/vm/vm.cpp
    src/vm/vm.hpp
    src/world/world.cpp
    src/world/refs.cpp
    src/world/lod.cpp
    src/world/lod.hpp
    src/world/quests.cpp
    src/world/world.hpp
    "${SKYDOT_WORLD_HEADER}"
    "${SKYDOT_SCRIPT_HEADER}"
    "${SKYDOT_LOD_HEADER}"
    "${SKYDOT_ANIMATION_HEADER}")

target_include_directories(skydot PRIVATE src "${SKYDOT_FB_DIR}")
target_include_directories(skydot SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/extern/flatbuffers/include")
target_link_libraries(skydot PRIVATE godot::cpp skydot::warnings)

get_target_property(_godotcpp_suffix godot-cpp GODOTCPP_SUFFIX)

# Output goes straight to game/bin/, where the .gdextension file looks, so with
# `reloadable = true` a rebuild reloads in the running editor. No install step.
set(_output_dir "${CMAKE_SOURCE_DIR}/game/bin")

set_target_properties(skydot PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON
    BUILD_RPATH_USE_ORIGIN ON
    LINK_SEARCH_START_STATIC ON
    LINK_SEARCH_END_STATIC ON
    # Stops multi-config generators (MSVC) from adding a per-config
    # subdirectory.
    LIBRARY_OUTPUT_DIRECTORY "$<1:${_output_dir}>"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${_output_dir}>"
    PDB_OUTPUT_DIRECTORY "$<1:${_output_dir}>"
    PREFIX "lib"
    OUTPUT_NAME "skydot${_godotcpp_suffix}")
