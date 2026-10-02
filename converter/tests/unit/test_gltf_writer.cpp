// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output is checked by parsing it back with fastgltf, which catches wrong
// strides or unaligned bufferViews that checks on our own structs would miss.
#include "bethconv/mesh/gltf_writer.hpp"

#include "bethconv/mesh/nif_reader.hpp"

#include "../support/nif_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>

namespace {

/// One cube, shaded, in the requested encoding, converted to a GLB.
std::vector<std::byte> cube_glb(bethconv::test::NifFlavor flavor,
                                const bethconv::mesh::WriteOptions& options = {}) {
    bethconv::test::NifBuilder builder(flavor);
    const auto cube = bethconv::test::make_cube();
    auto* shape = builder.add_shape("cube", cube);
    REQUIRE(shape != nullptr);
    builder.add_shader(shape, "textures\\test\\Diffuse.dds", "textures\\test\\Diffuse_n.dds");

    auto model = bethconv::mesh::read_nif(builder.bytes(), "cube.nif");
    REQUIRE(model.has_value());
    auto glb = bethconv::mesh::write_glb(*model, options);
    REQUIRE(glb.has_value());
    return std::move(*glb);
}

/// Parse a GLB back. The buffer is embedded, so the base directory is a
/// placeholder.
fastgltf::Asset parse(std::vector<std::byte>& glb) {
    fastgltf::Parser parser;
    auto data = fastgltf::GltfDataBuffer::FromBytes(glb.data(), glb.size());
    REQUIRE(data.error() == fastgltf::Error::None);
    auto asset = parser.loadGltfBinary(data.get(), std::filesystem::path("."),
                                       fastgltf::Options::None);
    REQUIRE(asset.error() == fastgltf::Error::None);
    return std::move(asset.get());
}

/// Read the JSON chunk directly, for parts fastgltf's Asset does not model
/// (extras are opaque strings without a parse callback).
nlohmann::json json_chunk(const std::vector<std::byte>& glb) {
    REQUIRE(glb.size() > 20);
    auto u32 = [&](std::size_t off) {
        return static_cast<std::uint32_t>(glb[off]) |
               static_cast<std::uint32_t>(glb[off + 1]) << 8 |
               static_cast<std::uint32_t>(glb[off + 2]) << 16 |
               static_cast<std::uint32_t>(glb[off + 3]) << 24;
    };
    const std::uint32_t chunk_length = u32(12);
    std::string text;
    text.reserve(chunk_length);
    for (std::uint32_t i = 0; i < chunk_length; ++i) {
        text.push_back(static_cast<char>(glb[20 + i]));
    }
    return nlohmann::json::parse(text);
}

/// The raw JSON chunk bytes, for cases where the text is not valid JSON.
std::string json_bytes(const std::vector<std::byte>& glb) {
    REQUIRE(glb.size() > 20);
    auto u32 = [&](std::size_t off) {
        return static_cast<std::uint32_t>(glb[off]) |
               static_cast<std::uint32_t>(glb[off + 1]) << 8 |
               static_cast<std::uint32_t>(glb[off + 2]) << 16 |
               static_cast<std::uint32_t>(glb[off + 3]) << 24;
    };
    const std::uint32_t chunk_length = u32(12);
    std::string text;
    text.reserve(chunk_length);
    for (std::uint32_t i = 0; i < chunk_length; ++i) {
        text.push_back(static_cast<char>(glb[20 + i]));
    }
    return text;
}

/// A cube with the given shape name and texture slots.
std::vector<std::byte> cube_glb_named(const std::string& shape_name,
                                      const std::string& diffuse,
                                      const std::string& normal) {
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    const auto cube = bethconv::test::make_cube();
    auto* shape = builder.add_shape(shape_name, cube);
    REQUIRE(shape != nullptr);
    builder.add_shader(shape, diffuse, normal);

    auto model = bethconv::mesh::read_nif(builder.bytes(), "cube.nif");
    REQUIRE(model.has_value());
    auto glb = bethconv::mesh::write_glb(*model);
    REQUIRE(glb.has_value());
    return std::move(*glb);
}

} // namespace

TEST_CASE("the output is a GLB that fastgltf will read back", "[gltf]") {
    auto glb = cube_glb(bethconv::test::NifFlavor::se);
    const auto asset = parse(glb);

    REQUIRE(asset.meshes.size() == 1);
    REQUIRE(asset.buffers.size() == 1);
    REQUIRE(asset.scenes.size() == 1);
    CHECK(asset.defaultScene.has_value());
}

TEST_CASE("geometry survives the round trip through glTF accessors", "[gltf]") {
    auto glb = cube_glb(bethconv::test::NifFlavor::se);
    const auto asset = parse(glb);

    const auto& prim = asset.meshes[0].primitives[0];
    const auto* position = prim.findAttribute("POSITION");
    REQUIRE(position != prim.attributes.end());
    const auto& accessor = asset.accessors[position->accessorIndex];
    CHECK(accessor.count == 8);
    CHECK(accessor.type == fastgltf::AccessorType::Vec3);
    CHECK(accessor.componentType == fastgltf::ComponentType::Float);

    REQUIRE(prim.indicesAccessor.has_value());
    CHECK(asset.accessors[*prim.indicesAccessor].count == 36);

    // Read values, not just counts: a correct count with a wrong byteOffset
    // passes every metadata check and scrambles the mesh.
    std::size_t seen = 0;
    fastgltf::iterateAccessor<fastgltf::math::fvec3>(
        asset, accessor, [&](fastgltf::math::fvec3 v) {
            CHECK_THAT(std::abs(v.x()), Catch::Matchers::WithinAbs(5.0, 0.001));
            CHECK_THAT(std::abs(v.y()), Catch::Matchers::WithinAbs(5.0, 0.001));
            CHECK_THAT(std::abs(v.z()), Catch::Matchers::WithinAbs(5.0, 0.001));
            ++seen;
        });
    CHECK(seen == 8);
}

TEST_CASE("POSITION carries min and max, which glTF requires", "[gltf]") {
    auto glb = cube_glb(bethconv::test::NifFlavor::se);
    const auto asset = parse(glb);
    const auto& prim = asset.meshes[0].primitives[0];
    const auto& accessor = asset.accessors[prim.findAttribute("POSITION")->accessorIndex];
    REQUIRE(accessor.min.has_value());
    REQUIRE(accessor.max.has_value());
    CHECK_THAT(accessor.min->get<double>(0), Catch::Matchers::WithinAbs(-5.0, 0.001));
    CHECK_THAT(accessor.max->get<double>(0), Catch::Matchers::WithinAbs(5.0, 0.001));
}

TEST_CASE("the axis conversion is a root node, not a baked vertex transform",
          "[gltf][axes]") {
    auto glb = cube_glb(bethconv::test::NifFlavor::se);
    const auto asset = parse(glb);

    // The scene has a single root: the pivot.
    REQUIRE(asset.scenes[0].nodeIndices.size() == 1);
    const auto& pivot = asset.nodes[asset.scenes[0].nodeIndices[0]];
    CHECK(std::string_view(pivot.name) == "bethconv_z_up_to_y_up");
    const auto* trs = std::get_if<fastgltf::TRS>(&pivot.transform);
    REQUIRE(trs != nullptr);
    CHECK_THAT(trs->scale.x(), Catch::Matchers::WithinAbs(0.0142875, 1e-6));
    // -90 degrees about X: quaternion (-sin45, 0, 0, cos45).
    CHECK_THAT(trs->rotation.x(), Catch::Matchers::WithinAbs(-0.70710678, 1e-5));
    CHECK_THAT(trs->rotation.w(), Catch::Matchers::WithinAbs(0.70710678, 1e-5));

    // Vertices are unchanged, so the mesh stays diffable against the NIF.
    const auto& accessor =
        asset.accessors[asset.meshes[0].primitives[0].findAttribute("POSITION")
                            ->accessorIndex];
    CHECK_THAT(accessor.max->get<double>(1), Catch::Matchers::WithinAbs(5.0, 0.001));
}

TEST_CASE("turning the conversion off leaves the model in NIF space", "[gltf][axes]") {
    bethconv::mesh::WriteOptions options;
    options.convert_to_y_up = false;
    options.unit_scale = 1.0f;
    auto glb = cube_glb(bethconv::test::NifFlavor::se, options);
    const auto asset = parse(glb);
    const auto& root = asset.nodes[asset.scenes[0].nodeIndices[0]];
    CHECK(std::string_view(root.name) != "bethconv_z_up_to_y_up");
}

TEST_CASE("Bethesda material data is preserved in extras rather than dropped",
          "[gltf][material]") {
    auto glb = cube_glb(bethconv::test::NifFlavor::se);
    const auto json = json_chunk(glb);

    REQUIRE(json.contains("materials"));
    const auto& material = json["materials"][0];
    REQUIRE(material.contains("extras"));
    const auto& extras = material["extras"]["bethconv"];
    CHECK(extras["shader"] == "BSLightingShaderProperty");
    // Slot roles are resolved and the raw path is kept too.
    CHECK(extras["texture_slots"]["0"]["role"] == "diffuse");
    CHECK(extras["texture_slots"]["0"]["path"] == "textures/test/diffuse.dds");
    CHECK(extras["texture_slots"]["1"]["role"] == "normal");
}

TEST_CASE("hair and skin tint colours reach the extras only when the shader has them",
          "[gltf][material]") {
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    auto* shape = builder.add_shape("cube", bethconv::test::make_cube());
    REQUIRE(shape != nullptr);
    builder.add_shader(shape, "textures\\test\\Diffuse.dds", "textures\\test\\Diffuse_n.dds");
    auto model = bethconv::mesh::read_nif(builder.bytes(), "cube.nif");
    REQUIRE(model.has_value());
    REQUIRE_FALSE(model->materials.empty());
    CHECK_FALSE(model->materials[0].hair_tint.has_value());

    const auto plain = json_chunk(*bethconv::mesh::write_glb(*model));
    CHECK_FALSE(plain["materials"][0]["extras"]["bethconv"].contains("hair_tint_color"));

    model->materials[0].hair_tint = bethconv::mesh::Vec3{0.5F, 0.25F, 0.125F};
    const auto tinted = json_chunk(*bethconv::mesh::write_glb(*model));
    const auto& colour = tinted["materials"][0]["extras"]["bethconv"]["hair_tint_color"];
    REQUIRE(colour.size() == 3);
    CHECK(colour[1].get<double>() == 0.25);
    CHECK_FALSE(tinted["materials"][0]["extras"]["bethconv"].contains("skin_tint_color"));
}

TEST_CASE("provenance rides on the root node, because asset extras do not survive",
          "[gltf][extras]") {
    // fastgltf 0.9.0 never writes Category::Asset extras, so provenance there
    // is lost silently. Catches a fix upstream or a regression here.
    auto glb = cube_glb(bethconv::test::NifFlavor::se);
    const auto json = json_chunk(glb);

    CHECK_FALSE(json["asset"].contains("extras"));

    const auto& nodes = json["nodes"];
    const auto pivot = std::ranges::find_if(nodes, [](const nlohmann::json& n) {
        return n.value("name", "") == "bethconv_z_up_to_y_up";
    });
    REQUIRE(pivot != nodes.end());
    REQUIRE(pivot->contains("extras"));
    const auto& provenance = (*pivot)["extras"]["bethconv"];
    CHECK(provenance["source"] == "cube.nif");
    CHECK(provenance["nif_stream_version"] == 100);
}

TEST_CASE("a billboard node carries its mode in extras", "[gltf][extras]") {
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    auto* glow = builder.add_node<nifly::NiBillboardNode>("glow");
    glow->billboardMode = nifly::ROTATE_ABOUT_UP;
    builder.add_node("plain");

    auto model = bethconv::mesh::read_nif(builder.bytes(), "fire.nif");
    REQUIRE(model.has_value());
    auto glb = bethconv::mesh::write_glb(*model);
    REQUIRE(glb.has_value());
    const auto json = json_chunk(*glb);

    const auto& nodes = json["nodes"];
    const auto named = [&](const std::string& name) {
        return *std::ranges::find_if(nodes, [&](const nlohmann::json& n) {
            return n.value("name", "") == name;
        });
    };
    CHECK(named("glow")["extras"]["bethconv"]["billboard"] == 1);
    CHECK_FALSE(named("plain").contains("extras"));
}

TEST_CASE("writing extras can be turned off entirely", "[gltf][extras]") {
    bethconv::mesh::WriteOptions options;
    options.write_extras = false;
    auto glb = cube_glb(bethconv::test::NifFlavor::se, options);
    const auto json = json_chunk(glb);
    CHECK_FALSE(json["materials"][0].contains("extras"));
    for (const auto& node : json["nodes"]) {
        CHECK_FALSE(node.contains("extras"));
    }
}

TEST_CASE("texture paths become lower-case, forward-slashed URIs", "[gltf][material]") {
    auto glb = cube_glb(bethconv::test::NifFlavor::se);
    const auto asset = parse(glb);
    REQUIRE(asset.images.size() == 2);
    const auto* uri = std::get_if<fastgltf::sources::URI>(&asset.images[0].data);
    REQUIRE(uri != nullptr);
    CHECK(uri->uri.string() == "textures/test/diffuse.dds");
}

TEST_CASE("a control byte in a texture slot still leaves a valid JSON document",
          "[gltf][material]") {
    // 67 of 22,573 vanilla SE FaceGen meshes reference `textures\<0x08>NOR`.
    // JSON forbids bytes below 0x20 in strings and fastgltf 0.9.0 only escapes
    // `"` and `\`; Godot loaded these files, Blender rejected them ("Invalid
    // control character").
    const std::string slot = std::string("textures\\") + '\x08' + "NOR";
    auto glb = cube_glb_named("cube", "textures\\test\\Diffuse.dds", slot);

    const std::string text = json_bytes(glb);
    for (const char c : text) {
        CHECK(static_cast<unsigned char>(c) >= 0x20);
    }
    // Parseable at all, as Blender requires.
    CHECK_NOTHROW([&] { return nlohmann::json::parse(text); }());
    CHECK(text.find("textures/%08nor") != std::string::npos);
}

TEST_CASE("an escaped URI still names the path it came from", "[gltf][material]") {
    // Escaping is reversible: percent-decoding (as glTF requires) gives the
    // original bytes.
    const std::string slot = std::string("textures\\") + '\x08' + "NOR";
    auto glb = cube_glb_named("cube", "textures\\house crafting\\nail01.dds", slot);

    const auto asset = parse(glb);
    REQUIRE(asset.images.size() == 2);
    const auto* diffuse = std::get_if<fastgltf::sources::URI>(&asset.images[0].data);
    const auto* normal = std::get_if<fastgltf::sources::URI>(&asset.images[1].data);
    REQUIRE(diffuse != nullptr);
    REQUIRE(normal != nullptr);
    CHECK(diffuse->uri.string() == "textures/house crafting/nail01.dds");
    CHECK(normal->uri.string() == std::string("textures/") + '\x08' + "nor");

    // 49 vanilla texture paths contain spaces, which must be escaped in a URI.
    CHECK(json_bytes(glb).find("house%20crafting/nail01.dds") != std::string::npos);
}

TEST_CASE("a slot that is not valid UTF-8 does not throw out of the writer",
          "[gltf][material]") {
    // Extras are built with nlohmann, whose dump() throws on invalid UTF-8;
    // cp1252 paths like `textures\caf\xE9\...` are common in mods. Previously
    // the whole mesh failed to convert over a byte in `extras`.
    const std::string slot = std::string("textures\\caf\xE9\\wood.dds");
    auto glb = cube_glb_named("cube", slot, "");

    const auto json = json_chunk(glb);
    const auto slots = json["materials"][0]["extras"]["bethconv"]["texture_slots"];
    CHECK(slots["0"]["path"] == "textures/caf%E9/wood.dds");

    // Valid UTF-8 is left as is.
    auto valid = cube_glb_named("cube", "textures\\caf\xC3\xA9\\wood.dds", "");
    const auto valid_json = json_chunk(valid);
    CHECK(valid_json["materials"][0]["extras"]["bethconv"]["texture_slots"]["0"]["path"] ==
          "textures/caf\xC3\xA9/wood.dds");
}

TEST_CASE("a texture URI resolves from the GLB, not from the pack root", "[gltf][material]") {
    // glTF resolves relative URIs against the document: `textures/test/
    // diffuse.dds` in `meshes/armor/x.glb` means `meshes/armor/textures/...`.
    // Godot reported 1,687 missing images on a 350-file import this way.
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    const auto cube = bethconv::test::make_cube();
    auto* shape = builder.add_shape("cube", cube);
    REQUIRE(shape != nullptr);
    builder.add_shader(shape, "textures\\test\\Diffuse.dds", "");

    auto model = bethconv::mesh::read_nif(builder.bytes(), "meshes/armor/iron/x.nif");
    REQUIRE(model.has_value());
    model->source = "meshes/armor/iron/x.nif";
    auto glb = bethconv::mesh::write_glb(*model);
    REQUIRE(glb.has_value());

    const auto asset = parse(*glb);
    REQUIRE(asset.images.size() == 1);
    const auto* uri = std::get_if<fastgltf::sources::URI>(&asset.images[0].data);
    REQUIRE(uri != nullptr);
    CHECK(uri->uri.string() == "../../../textures/test/diffuse.dds");

    // extras keep the pack-relative form; it names the texture.
    const auto json = json_chunk(*glb);
    const auto extras = json["materials"][0]["extras"]["bethconv"];
    CHECK(extras["texture_slots"]["0"]["path"] == "textures/test/diffuse.dds");
}

TEST_CASE("a model with nothing in it still produces a loadable glTF", "[gltf]") {
    // A bare node tree (skeleton, animation carrier) has no shapes and must
    // still produce a valid GLB.
    bethconv::mesh::Model empty;
    empty.source = "empty.nif";
    empty.nif_version = "test";
    bethconv::mesh::Node root;
    root.name = "root";
    empty.nodes.push_back(std::move(root));
    empty.roots.push_back(0);

    auto glb = bethconv::mesh::write_glb(empty);
    REQUIRE(glb.has_value());
    const auto asset = parse(*glb);
    CHECK(asset.meshes.empty());
    CHECK(asset.scenes.size() == 1);
}

TEST_CASE("a non-finite value never reaches the document as a number", "[gltf]") {
    // JSON has no NaN, and fastgltf 0.9.0 writes it as `2.696539702293474e+308`
    // without an error. Vanilla SE contains one (greybeardstatic.nif).
    const float nan = std::numeric_limits<float>::quiet_NaN();

    bethconv::mesh::Model model;
    model.source = "meshes/x.nif";
    model.nif_version = "test";
    bethconv::mesh::Node root;
    root.name = "root";
    root.transform.translation = bethconv::mesh::Vec3{nan, nan, nan};
    root.transform.scale =
        bethconv::mesh::Vec3{std::numeric_limits<float>::infinity(), 1.0f, 1.0f};
    model.nodes.push_back(std::move(root));
    model.roots.push_back(0);

    auto glb = bethconv::mesh::write_glb(model);
    REQUIRE(glb.has_value());

    // Check the text; fastgltf's own parser accepts what it writes.
    const auto text = json_bytes(*glb);
    CHECK(text.find("e+308") == std::string::npos);
    CHECK(nlohmann::json::accept(text));

    const auto json = json_chunk(*glb);
    // The node stays; only its transform becomes identity.
    const auto& node = json["nodes"][0];
    if (node.contains("translation")) {
        for (const auto& v : node["translation"]) {
            CHECK(v.get<double>() == 0.0);
        }
    }
}

TEST_CASE("refraction surfaces are written fully transparent", "[mesh][gltf][material]") {
    // A heat-haze plane uses a normal map in its diffuse slot; drawn as a
    // normal surface it shows the normal map's colors.
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    auto* shader = builder.add_shader(builder.add_shape("haze", bethconv::test::make_cube()),
                                      "textures\\effects\\vaportilenormal_n.dds");
    shader->shaderFlags1 |= nifly::SLSF1_REFRACTION;
    auto model = bethconv::mesh::read_nif(builder.bytes(), "fire.nif");
    REQUIRE(model.has_value());
    auto glb = bethconv::mesh::write_glb(*model, {});
    REQUIRE(glb.has_value());

    const auto json = json_chunk(*glb);
    const auto& mat = json["materials"][0];
    CHECK(mat["alphaMode"] == "BLEND");
    CHECK(mat["pbrMetallicRoughness"]["baseColorFactor"][3] == 0.0);
    CHECK(mat["extras"]["bethconv"]["refraction"] == true);
}

namespace {

/// The collision extras of the node that has them.
nlohmann::json collision_extras(const std::vector<std::byte>& glb) {
    const auto json = json_chunk(glb);
    for (const auto& node : json["nodes"]) {
        if (node.contains("extras") && node["extras"]["bethconv"].contains("collision")) {
            return node["extras"]["bethconv"]["collision"];
        }
    }
    return {};
}

} // namespace

TEST_CASE("collision keeps the rigid body's layer, quality, mass and friction",
          "[mesh][gltf][collision]") {
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    builder.add_shape("cube", bethconv::test::make_cube());
    auto box = std::make_unique<nifly::bhkBoxShape>();
    box->dimensions = nifly::Vector3(0.5f, 0.25f, 1.0f);
    box->radius = 0.05f;
    bethconv::test::NifBuilder::Body body;
    body.layer = 4;
    body.quality = 4;
    body.mass = 3.0f;
    body.friction = 0.7f;
    builder.add_collision(std::move(box), body);

    auto model = bethconv::mesh::read_nif(builder.bytes(), "clutter.nif");
    REQUIRE(model.has_value());
    auto glb = bethconv::mesh::write_glb(*model, {});
    REQUIRE(glb.has_value());

    const auto shapes = collision_extras(*glb);
    REQUIRE(shapes.size() == 1);
    const auto& shape = shapes[0];
    CHECK(shape["kind"] == "box");
    CHECK(shape["layer"] == 4);
    CHECK(shape["quality_type"] == 4);
    CHECK_THAT(shape["mass"].get<double>(), Catch::Matchers::WithinAbs(3.0, 1e-6));
    CHECK_THAT(shape["friction"].get<double>(), Catch::Matchers::WithinAbs(0.7, 1e-6));
    // Havok units, unscaled: the engine converts.
    CHECK_THAT(shape["half_extents"][2].get<double>(), Catch::Matchers::WithinAbs(1.0, 1e-6));
    CHECK_THAT(shape["radius"].get<double>(), Catch::Matchers::WithinAbs(0.05, 1e-6));
}

TEST_CASE("a transform shape inside a bhkRigidBodyT adds to the body's transform",
          "[mesh][nif][collision]") {
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    builder.add_shape("cube", bethconv::test::make_cube());
    auto box = std::make_unique<nifly::bhkBoxShape>();
    box->dimensions = nifly::Vector3(1.0f, 1.0f, 1.0f);
    bethconv::test::NifBuilder::Body body;
    body.translation = nifly::Vector3(1.0f, 0.0f, 0.0f);
    builder.add_collision(std::move(box), body, nullptr, nifly::Vector3(0.0f, 2.0f, 0.0f));

    auto model = bethconv::mesh::read_nif(builder.bytes(), "moved.nif");
    REQUIRE(model.has_value());
    REQUIRE(model->collision.size() == 1);
    const auto& t = model->collision[0].transform.translation;
    CHECK_THAT(t.x, Catch::Matchers::WithinAbs(1.0, 1e-6));
    CHECK_THAT(t.y, Catch::Matchers::WithinAbs(2.0, 1e-6));
    CHECK_THAT(t.z, Catch::Matchers::WithinAbs(0.0, 1e-6));
}

TEST_CASE("compressed-mesh chunks keep the triangle list that follows their strips",
          "[mesh][nif][collision]") {
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    builder.add_shape("cube", bethconv::test::make_cube());

    // One chunk, a unit square (1/1000 Havok units per step): a strip of one
    // triangle, then one more triangle as a plain list.
    // nifly's push_back takes a non-const reference.
    nifly::bhkCMSDChunk chunk;
    const std::vector<std::uint16_t> verts{0, 0, 0, 1000, 0, 0, 0, 1000, 0, 1000, 1000, 0};
    const std::vector<std::uint16_t> indices{0, 1, 2, 1, 3, 2};
    for (std::uint16_t v : verts) {
        chunk.verts.push_back(v);
    }
    for (std::uint16_t i : indices) {
        chunk.indices.push_back(i);
    }
    std::uint16_t strip = 3;
    chunk.strips.push_back(strip);
    auto data = std::make_unique<nifly::bhkCompressedMeshShapeData>();
    data->chunks.push_back(chunk);
    auto& header = builder.file().GetHeader();
    const auto data_index = header.AddBlock(std::move(data));
    auto mesh = std::make_unique<nifly::bhkCompressedMeshShape>();
    mesh->dataRef.index = data_index;
    builder.add_collision(std::move(mesh), {});

    auto model = bethconv::mesh::read_nif(builder.bytes(), "square.nif");
    REQUIRE(model.has_value());
    REQUIRE(model->collision.size() == 1);
    const auto& shape = model->collision[0];
    CHECK(shape.kind == bethconv::mesh::CollisionKind::compressed_mesh);
    CHECK(shape.vertices.size() == 4);
    CHECK(shape.indices == std::vector<std::uint32_t>{0, 1, 2, 1, 3, 2});
}

TEST_CASE("a Havok cylinder keeps its ends and radius, padded by its convex radius",
          "[mesh][gltf][collision]") {
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    builder.add_shape("cube", bethconv::test::make_cube());
    auto cylinder = std::make_unique<nifly::bhkCylinderShape>();
    cylinder->vertexA = nifly::Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    cylinder->vertexB = nifly::Vector4(0.0f, 0.0f, 1.0f, 0.0f);
    cylinder->cylinderRadius = 0.5f;
    cylinder->radius = 0.1f;
    builder.add_collision(std::move(cylinder), {});

    auto model = bethconv::mesh::read_nif(builder.bytes(), "well.nif");
    REQUIRE(model.has_value());
    CHECK(model->warnings.empty());
    auto glb = bethconv::mesh::write_glb(*model, {});
    REQUIRE(glb.has_value());
    const auto shapes = collision_extras(*glb);
    REQUIRE(shapes.size() == 1);
    CHECK(shapes[0]["kind"] == "cylinder");
    CHECK_THAT(shapes[0]["radius"].get<double>(), Catch::Matchers::WithinAbs(0.6, 1e-6));
    CHECK_THAT(shapes[0]["point_a"][2].get<double>(), Catch::Matchers::WithinAbs(-0.1, 1e-6));
    CHECK_THAT(shapes[0]["point_b"][2].get<double>(), Catch::Matchers::WithinAbs(1.1, 1e-6));
}

TEST_CASE("bhkNiTriStripsShape becomes a mesh in Havok units", "[mesh][gltf][collision]") {
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    builder.add_shape("cube", bethconv::test::make_cube());
    // A square of 70 game units as one strip of two triangles.
    auto data = std::make_unique<nifly::NiTriStripsData>();
    const std::vector<nifly::Vector3> verts{
        {0.0f, 0.0f, 0.0f}, {70.0f, 0.0f, 0.0f}, {0.0f, 70.0f, 0.0f}, {70.0f, 70.0f, 0.0f}};
    auto version = builder.file().GetHeader().GetVersion();
    data->Create(version, &verts, nullptr, nullptr, nullptr);
    std::uint16_t length = 4;
    data->stripsInfo.stripLengths.push_back(length);
    data->stripsInfo.points = {{0, 1, 2, 3}};
    auto& header = builder.file().GetHeader();
    const auto data_index = header.AddBlock(std::move(data));
    auto strips = std::make_unique<nifly::bhkNiTriStripsShape>();
    strips->partRefs.AddBlockRef(data_index);
    builder.add_collision(std::move(strips), {});

    auto model = bethconv::mesh::read_nif(builder.bytes(), "pelt.nif");
    REQUIRE(model.has_value());
    CHECK(model->warnings.empty());
    REQUIRE(model->collision.size() == 1);
    const auto& shape = model->collision[0];
    CHECK(shape.kind == bethconv::mesh::CollisionKind::mesh);
    REQUIRE(shape.vertices.size() == 4);
    CHECK_THAT(shape.vertices[3].x, Catch::Matchers::WithinAbs(70.0 / 69.99124, 1e-5));
    CHECK(shape.indices.size() == 6);

    auto glb = bethconv::mesh::write_glb(*model, {});
    REQUIRE(glb.has_value());
    const auto shapes = collision_extras(*glb);
    REQUIRE(shapes.size() == 1);
    CHECK(shapes[0]["kind"] == "mesh");
    CHECK(shapes[0]["indices"].size() == 6);
}

TEST_CASE("a bhkPlaneShape becomes the flat hull where it cuts its bounds",
          "[mesh][nif][collision]") {
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    builder.add_shape("cube", bethconv::test::make_cube());
    auto plane = std::make_unique<nifly::bhkPlaneShape>();
    plane->plane.normal = nifly::Vector3(0.0f, 0.0f, 1.0f);
    plane->plane.constant = -0.25f; // z = 0.25
    plane->center = nifly::Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    plane->halfExtents = nifly::Vector4(1.0f, 2.0f, 1.0f, 0.0f);
    builder.add_collision(std::move(plane), {});

    auto model = bethconv::mesh::read_nif(builder.bytes(), "eggs.nif");
    REQUIRE(model.has_value());
    REQUIRE(model->collision.size() == 1);
    const auto& shape = model->collision[0];
    CHECK(shape.kind == bethconv::mesh::CollisionKind::convex_vertices);
    REQUIRE(shape.vertices.size() == 4);
    for (const auto& v : shape.vertices) {
        CHECK_THAT(v.z, Catch::Matchers::WithinAbs(0.25, 1e-6));
        CHECK_THAT(std::abs(v.x), Catch::Matchers::WithinAbs(1.0, 1e-6));
        CHECK_THAT(std::abs(v.y), Catch::Matchers::WithinAbs(2.0, 1e-6));
    }
}
