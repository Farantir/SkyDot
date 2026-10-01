# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(bethconv_unit_tests
    test_span_reader.cpp
    test_json_text.cpp
    test_mapped_file.cpp
    test_record_plugin.cpp
    test_load_order.cpp
    test_vpath.cpp
    test_archive_set.cpp
    test_byte_writer.cpp
    test_span_stream.cpp
    test_nif_reader.cpp
    test_nif_animation.cpp
    test_gltf_writer.cpp
    test_dds.cpp
    test_mip_tail.cpp
    test_forms.cpp
    test_vmad.cpp
    test_quest.cpp
    test_lod_asset.cpp
    test_strings.cpp
    test_merge.cpp
    test_snapshot.cpp
    test_pex.cpp
    test_pex_script.cpp
    test_content_hash.cpp
    test_pack_writer.cpp
    test_pack_view.cpp
    test_convert.cpp
    test_deflate.cpp
    test_world.cpp
)

# The synthetic-plugin builder in tests/support deflates payloads directly.
find_package(ZLIB REQUIRED)

# test_deflate.cpp builds its own LZ4 frames, for the reason its header explains.
find_package(lz4 CONFIG REQUIRED)

# Archive fixtures are written with rsm-bsa (see tests/support/bsa_builder.hpp).
find_package(bsa CONFIG REQUIRED)

# Mesh fixtures are written with nifly; the emitted GLB is read back with
# fastgltf.
find_package(fastgltf CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(bethconv_unit_tests
    PRIVATE bethconv::core bethconv::warnings Catch2::Catch2WithMain ZLIB::ZLIB lz4::lz4 bsa::bsa
            nifly fastgltf::fastgltf nlohmann_json::nlohmann_json)

catch_discover_tests(bethconv_unit_tests)
