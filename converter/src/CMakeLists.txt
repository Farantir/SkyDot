# SPDX-License-Identifier: GPL-3.0-or-later
find_package(ZLIB REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(bsa CONFIG REQUIRED)
find_package(fastgltf CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(flatbuffers CONFIG REQUIRED)
# Content hashing for the pack.
find_package(blake3 CONFIG REQUIRED)

# records_generated.h is generated at build time so it cannot drift from
# formats/schema/records.fbs. flatc comes from the vcpkg port.
set(BETHCONV_FB_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/bethconv/pack")
set(BETHCONV_FB_HEADERS "")
foreach(schema IN ITEMS records world script lod animation)
    set(fbs "${CMAKE_CURRENT_SOURCE_DIR}/../../formats/schema/${schema}.fbs")
    set(header "${BETHCONV_FB_DIR}/${schema}_generated.h")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${BETHCONV_FB_DIR}"
        COMMAND flatbuffers::flatc --cpp --cpp-std c++17 --scoped-enums
                -o "${BETHCONV_FB_DIR}" "${fbs}"
        DEPENDS "${fbs}" flatbuffers::flatc
        COMMENT "flatc: ${schema}.fbs -> ${schema}_generated.h"
        VERBATIM)
    list(APPEND BETHCONV_FB_HEADERS "${header}")
endforeach()
add_custom_target(bethconv_records_fbs DEPENDS ${BETHCONV_FB_HEADERS})

add_library(bethconv_core STATIC
    io/span_reader.cpp
    io/mapped_file.cpp
    io/deflate.cpp
    io/byte_writer.cpp
    io/span_stream.cpp
    io/output_target.cpp
    io/json_text.cpp
    install/vdf.cpp
    install/game_install.cpp
    install/mo2.cpp
    install/mount_plan.cpp
    record/types.cpp
    record/headers.cpp
    record/plugin.cpp
    record/load_order.cpp
    record/histogram.cpp
    record/field_reader.cpp
    record/conditions.cpp
    record/vmad.cpp
    record/forms.cpp
    record/forms_object.cpp
    record/forms_world.cpp
    record/forms_game.cpp
    record/forms_actor.cpp
    record/form_census.cpp
    record/strings.cpp
    record/merge.cpp
    archive/vpath.cpp
    archive/archive_set.cpp
    mesh/nif_reader.cpp
    mesh/nif_controllers.cpp
    mesh/gltf_writer.cpp
    animation/animation_data.cpp
    animation/hkx.cpp
    texture/dds.cpp
    texture/mip_tail.cpp
    texture/mip_drop.cpp
    texture/bc_encode.cpp
    pack/snapshot_writer.cpp
    pack/snapshot_reader.cpp
    pack/content_hash.cpp
    pack/asset_store.cpp
    pack/pack_writer.cpp
    pack/script_asset.cpp
    pack/lod_asset.cpp
    pack/animation_asset.cpp
    pack/vpath_index.cpp
    pack/pack_view.cpp
    pack/inputs.cpp
    pack/asset_conversion.cpp
    pack/convert.cpp
    pack/world.cpp
    pack/world/context.cpp
    pack/world/fb_write.cpp
    pack/world_file.cpp
    script/pex.cpp
    script/pex_script.cpp
    ${BETHCONV_FB_HEADERS}
)
add_dependencies(bethconv_core bethconv_records_fbs)
add_library(bethconv::core ALIAS bethconv_core)

target_include_directories(bethconv_core
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    # Private: pack/snapshot.hpp hides FlatBuffers behind a pImpl.
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_link_libraries(bethconv_core
    PRIVATE bethconv::warnings ZLIB::ZLIB lz4::lz4 bsa::bsa nifly fastgltf::fastgltf nlohmann_json::nlohmann_json
            flatbuffers::flatbuffers BLAKE3::blake3 bc7enc
    PUBLIC bethconv::sanitizers)

# install/game_install.cpp reads Steam's and the games' registry keys.
if(WIN32)
    target_link_libraries(bethconv_core PRIVATE advapi32)
endif()

set_target_properties(bethconv_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
