// SPDX-License-Identifier: GPL-3.0-or-later
//
// The same mesh encoded the LE and SE way must give the same IR, so one code
// path serves both. The fixture builder generates both encodings from one
// source.
#include "bethconv/mesh/nif_reader.hpp"

#include "../support/nif_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

using bethconv::mesh::AlphaMode;
using bethconv::mesh::NifFlavor;
using bethconv::mesh::read_nif;
using bethconv::test::CubeMesh;
using bethconv::test::make_cube;
using bethconv::test::NifBuilder;

namespace {

/// Builds a one-cube NIF in the requested encoding and reads it back.
bethconv::mesh::Model round_trip(bethconv::test::NifFlavor flavor,
                                 const std::string& name = "cube") {
    NifBuilder builder(flavor);
    const CubeMesh cube = make_cube();
    auto* shape = builder.add_shape(name, cube);
    REQUIRE(shape != nullptr);
    builder.add_shader(shape, "textures/test/diffuse.dds", "textures/test/diffuse_n.dds");

    const auto bytes = builder.bytes();
    REQUIRE_FALSE(bytes.empty());

    auto model = read_nif(bytes, "test.nif");
    REQUIRE(model.has_value());
    return std::move(*model);
}

} // namespace

TEST_CASE("the LE and SE encodings of one mesh read back identically", "[mesh][flavor]") {
    const auto le = round_trip(bethconv::test::NifFlavor::le);
    const auto se = round_trip(bethconv::test::NifFlavor::se);

    // Only the stream version differs; the version strings are identical.
    CHECK(le.nif_version == se.nif_version);
    CHECK(le.nif_stream == 83);
    CHECK(se.nif_stream == 100);
    CHECK(bethconv::mesh::flavor_of(le.nif_stream) == NifFlavor::le);
    CHECK(bethconv::mesh::flavor_of(se.nif_stream) == NifFlavor::se);

    REQUIRE(le.primitives.size() == 1);
    REQUIRE(se.primitives.size() == 1);
    const auto& a = le.primitives[0];
    const auto& b = se.primitives[0];

    CHECK(a.positions.size() == b.positions.size());
    CHECK(a.indices.size() == b.indices.size());
    CHECK(a.uvs.size() == b.uvs.size());
    CHECK(a.normals.size() == b.normals.size());

    // Positions must match numerically. SE uses half floats, so not exactly,
    // but within a millimetre on a ten-unit cube.
    for (std::size_t i = 0; i < a.positions.size(); ++i) {
        CHECK_THAT(a.positions[i].x,
                   Catch::Matchers::WithinAbs(b.positions[i].x, 0.001));
        CHECK_THAT(a.positions[i].y,
                   Catch::Matchers::WithinAbs(b.positions[i].y, 0.001));
        CHECK_THAT(a.positions[i].z,
                   Catch::Matchers::WithinAbs(b.positions[i].z, 0.001));
    }
    CHECK(a.indices == b.indices);
}

TEST_CASE("flavor_of names the two encodings and refuses to guess", "[mesh][flavor]") {
    CHECK(bethconv::mesh::flavor_of(83) == NifFlavor::le);
    CHECK(bethconv::mesh::flavor_of(100) == NifFlavor::se);
    // VR and later titles raise the stream version above 100 with the same
    // geometry encoding, hence >=.
    CHECK(bethconv::mesh::flavor_of(130) == NifFlavor::se);
    // Vanilla LE ships a stream-34 file (meshes/weapons/2handmelee/2handsword.nif,
    // a Fallout 3 bat); it is reported as unknown, not guessed.
    CHECK(bethconv::mesh::flavor_of(34) == NifFlavor::unknown);
    CHECK(bethconv::mesh::flavor_of(0) == NifFlavor::unknown);
}

TEST_CASE("every per-vertex attribute is either absent or full length", "[mesh]") {
    const auto model = round_trip(bethconv::test::NifFlavor::se);
    const auto& prim = model.primitives[0];
    const std::size_t n = prim.positions.size();

    REQUIRE(n > 0);
    for (const auto& [name, size] : std::array<std::pair<const char*, std::size_t>, 4>{
             {{"normals", prim.normals.size()},
              {"tangents", prim.tangents.size()},
              {"uvs", prim.uvs.size()},
              {"colors", prim.colors.size()}}}) {
        INFO(name);
        CHECK((size == 0 || size == n));
    }
}

TEST_CASE("triangle indices never point past the vertex array", "[mesh]") {
    const auto model = round_trip(bethconv::test::NifFlavor::se);
    const auto& prim = model.primitives[0];
    CHECK(prim.indices.size() % 3 == 0);
    CHECK(std::ranges::all_of(prim.indices, [&](std::uint32_t i) {
        return i < prim.positions.size();
    }));
}

TEST_CASE("texture slots survive verbatim and stay numbered", "[mesh][material]") {
    const auto model = round_trip(bethconv::test::NifFlavor::se);
    REQUIRE(model.materials.size() == 1);
    const auto& mat = model.materials[0];
    CHECK(mat.kind == bethconv::mesh::ShaderKind::lighting);
    // Backslashes: nifly normalizes texture paths to Bethesda's separator on
    // save. The IR keeps the file's form; URI conversion is the writer's job.
    CHECK(mat.textures[0] == "textures\\test\\diffuse.dds");
    CHECK(mat.textures[1] == "textures\\test\\diffuse_n.dds");
    // Unfilled slots stay empty.
    CHECK(mat.textures[2].empty());
}

TEST_CASE("NiAlphaProperty's bitfield becomes a glTF alpha mode", "[mesh][material]") {
    auto build = [](std::uint16_t flags, std::uint8_t threshold) {
        NifBuilder builder(bethconv::test::NifFlavor::se);
        const CubeMesh cube = make_cube();
        auto* shape = builder.add_shape("cube", cube);
        builder.add_shader(shape, "textures/test/diffuse.dds");
        builder.add_alpha(shape, flags, threshold);
        auto model = read_nif(builder.bytes(), "test.nif");
        REQUIRE(model.has_value());
        REQUIRE(model->materials.size() == 1);
        return model->materials[0];
    };

    SECTION("alpha testing becomes MASK and carries the threshold") {
        const auto mat = build(0x0201, 128);
        CHECK(mat.alpha_mode == AlphaMode::mask);
        CHECK_THAT(mat.alpha_cutoff, Catch::Matchers::WithinAbs(128.0 / 255.0, 0.001));
    }
    SECTION("blending without testing becomes BLEND") {
        const auto mat = build(0x0001, 0);
        CHECK(mat.alpha_mode == AlphaMode::blend);
    }
    SECTION("both set becomes MASK, because glTF cannot say both") {
        // Vanilla foliage and chain-link set both; MASK renders them correctly,
        // BLEND sorts them wrongly.
        const auto mat = build(0x12ec, 80);
        CHECK(mat.alpha_mode == AlphaMode::mask);
        CHECK_THAT(mat.alpha_cutoff, Catch::Matchers::WithinAbs(80.0 / 255.0, 0.001));
    }
    SECTION("no alpha property at all is opaque") {
        NifBuilder builder(bethconv::test::NifFlavor::se);
        const CubeMesh cube = make_cube();
        auto* shape = builder.add_shape("cube", cube);
        builder.add_shader(shape, "textures/test/diffuse.dds");
        auto model = read_nif(builder.bytes(), "test.nif");
        REQUIRE(model.has_value());
        CHECK(model->materials[0].alpha_mode == AlphaMode::opaque);
        CHECK_FALSE(model->materials[0].alpha_property_present);
    }
}

TEST_CASE("tangents follow U, and the bitangent points up the image", "[mesh]") {
    // Bethesda's tangent arrays are swapped relative to their names; nifly's
    // tangent generation reproduces the game's layout. A quad in the XY plane
    // with v running down the image (towards -Y), as in a DDS.
    const std::vector<nifly::Vector3> verts{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    const std::vector<nifly::Vector2> uvs{{0, 1}, {1, 1}, {0, 0}, {1, 0}};
    const std::vector<nifly::Vector3> normals(4, nifly::Vector3(0, 0, 1));
    const std::vector<nifly::Triangle> tris{{0, 1, 2}, {1, 3, 2}};

    for (const auto flavor : {bethconv::test::NifFlavor::le, bethconv::test::NifFlavor::se}) {
        NifBuilder builder(flavor);
        auto* shape =
            builder.file().CreateShapeFromData("quad", &verts, &tris, &uvs, &normals);
        REQUIRE(shape != nullptr);
        builder.add_shader(shape, "textures/test/diffuse.dds", "textures/test/diffuse_n.dds");
        builder.file().CalcTangentsForShape(shape);

        const auto model = read_nif(builder.bytes(), "quad.nif");
        REQUIRE(model.has_value());
        REQUIRE(model->primitives.size() == 1);
        const auto& prim = model->primitives[0];
        REQUIRE(prim.tangents.size() == 4);
        for (const auto& t : prim.tangents) {
            // Tangent along +X (increasing u); normal x tangent = +Y, which is
            // up the image, so w is +1.
            CHECK(t.x > 0.9f);
            CHECK(std::abs(t.y) < 0.1f);
            CHECK(t.w == 1.0f);
        }
    }
}

TEST_CASE("a shape with no name is given one rather than left blank", "[mesh]") {
    const auto model = round_trip(bethconv::test::NifFlavor::se, "");
    REQUIRE(model.primitives.size() == 1);
    // Vanilla trees and effect planes have unnamed shapes; glTF tools select by
    // name.
    CHECK_FALSE(model.primitives[0].name.empty());
}

TEST_CASE("malformed input fails instead of throwing or crashing", "[mesh][robustness]") {
    SECTION("empty") {
        const auto model = read_nif({}, "empty.nif");
        REQUIRE_FALSE(model.has_value());
        CHECK(model.error().kind == bethconv::io::ErrorKind::truncated);
    }
    SECTION("not a NIF at all") {
        const std::array<std::byte, 8> junk{std::byte{'n'}, std::byte{'o'}, std::byte{'p'},
                                            std::byte{'e'}, std::byte{0},   std::byte{0},
                                            std::byte{0},   std::byte{0}};
        const auto model = read_nif(junk, "junk.nif");
        CHECK_FALSE(model.has_value());
    }
    SECTION("a real NIF truncated at every length still never throws") {
        NifBuilder builder(bethconv::test::NifFlavor::se);
        const CubeMesh cube = make_cube();
        builder.add_shader(builder.add_shape("cube", cube), "t.dds");
        const auto whole = builder.bytes();
        REQUIRE(whole.size() > 64);

        // Every prefix of a valid file is a plausible corrupt input and none may
        // crash. Step 7 keeps it fast while hitting every header field.
        for (std::size_t len = 0; len < whole.size(); len += 7) {
            const auto model = read_nif(std::span(whole).first(len), "cut.nif");
            if (model.has_value()) {
        // Accepting a prefix is fine; producing nonsense is not.
                for (const auto& prim : model->primitives) {
                    CHECK(std::ranges::all_of(prim.indices, [&](std::uint32_t i) {
                        return i < prim.positions.size();
                    }));
                }
            }
        }
    }
}

// ---- adversarial scene graphs ---------------------------------------------
//
// Not found in vanilla, but describable by untrusted files: child references
// are block indices that may point back up the tree. Both found by fuzz_nif
// (the first as a stack overflow from a 291-byte file).

TEST_CASE("a scene graph deeper than the limit is truncated, not fatal",
          "[mesh][nif]") {
    NifBuilder builder(bethconv::test::NifFlavor::se);
    nifly::NiNode* parent = nullptr;
    // Well past k_max_node_depth (256).
    for (int i = 0; i < 400; ++i) {
        parent = builder.add_node("deep" + std::to_string(i), parent);
    }

    const auto model = read_nif(builder.bytes(), "deep.nif");
    REQUIRE(model.has_value());
    // Nodes down to the limit are kept and the truncation is reported.
    CHECK(model->nodes.size() > 100);
    CHECK(model->nodes.size() < 400);
    const bool warned = std::ranges::any_of(model->warnings, [](const std::string& w) {
        return w.find("deeper than") != std::string::npos;
    });
    CHECK(warned);
}

TEST_CASE("a cyclic scene graph is broken, not walked forever", "[mesh][nif]") {
    // A depth limit alone would give 256 duplicated nodes; the cycle check
    // gives one node and a warning.
    NifBuilder builder(bethconv::test::NifFlavor::se);
    auto* a = builder.add_node("a");
    auto* b = builder.add_node("b", a);
    builder.add_child_ref(b, a); // b's child is its own grandparent

    const auto model = read_nif(builder.bytes(), "cycle.nif");
    REQUIRE(model.has_value());
    CHECK(model->nodes.size() < 10);
    const bool warned = std::ranges::any_of(model->warnings, [](const std::string& w) {
        return w.find("cycle broken") != std::string::npos;
    });
    CHECK(warned);
}

TEST_CASE("a NaN node transform is named and replaced, not carried", "[mesh][nif]") {
    // Vanilla SE's greybeardstatic.nif has a NaN translation (three 0xFFFFFFFF
    // words) on node `AnimObjectR`, which nifly passes through. It must not
    // spread through the IR or reach JSON.
    NifBuilder builder(bethconv::test::NifFlavor::se);
    auto* node = builder.add_node("AnimObjectR");
    REQUIRE(node != nullptr);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    node->transform.translation = nifly::Vector3(nan, nan, nan);

    const auto model = read_nif(builder.bytes(), "nan.nif");
    REQUIRE(model.has_value());

    const auto found = std::ranges::find_if(
        model->nodes, [](const bethconv::mesh::Node& n) { return n.name == "AnimObjectR"; });
    REQUIRE(found != model->nodes.end());
    CHECK(std::isfinite(found->transform.translation.x));
    CHECK(found->transform.translation.x == 0.0f);

    // The rewrite must be reported, naming the node.
    const bool warned = std::ranges::any_of(model->warnings, [](const std::string& w) {
        return w.find("non-finite transform") != std::string::npos &&
               w.find("AnimObjectR") != std::string::npos;
    });
    CHECK(warned);
}

TEST_CASE("EditorMarker geometry is dropped", "[mesh]") {
    // Cobwebs, invisible chairs and trigger meshes carry a shape named
    // "EditorMarker" that only the Creation Kit shows.
    NifBuilder builder(bethconv::test::NifFlavor::se);
    const CubeMesh cube = make_cube();
    builder.add_shader(builder.add_shape("web", cube), "textures/test/web.dds");
    builder.add_shader(builder.add_shape("EditorMarker", cube), "textures/test/marker.dds");
    const auto bytes = builder.bytes();

    const auto dropped = read_nif(bytes, "web.nif");
    REQUIRE(dropped.has_value());
    CHECK(dropped->primitives.size() == 1);
    CHECK(dropped->primitives[0].name == "web");

    const auto kept = read_nif(bytes, "web.nif", bethconv::mesh::ReadOptions{.skip_editor_markers = false});
    REQUIRE(kept.has_value());
    CHECK(kept->primitives.size() == 2);
}

TEST_CASE("shader flags are read, and refraction is recognized", "[mesh][material]") {
    NifBuilder builder(bethconv::test::NifFlavor::se);
    auto* shader = builder.add_shader(builder.add_shape("haze", make_cube()),
                                      "textures/effects/vaportilenormal_n.dds");
    shader->shaderFlags1 |= nifly::SLSF1_REFRACTION;
    const auto model = read_nif(builder.bytes(), "fire.nif");
    REQUIRE(model.has_value());
    REQUIRE(model->materials.size() == 1);
    CHECK((model->materials[0].shader_flags1 & nifly::SLSF1_REFRACTION) != 0);
    CHECK(model->materials[0].refraction);
}
