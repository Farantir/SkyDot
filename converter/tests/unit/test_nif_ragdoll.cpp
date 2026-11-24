// SPDX-License-Identifier: GPL-3.0-or-later
//
// A skeleton's ragdoll: the reader collects the rigid bodies and constraints
// of a NIF built in memory the way the game's skeleton.nif files are laid out
// (bhkBlendCollisionObject bodies storing their bone's bind transform, A the
// child, B the parent), and the writer puts them into the root extras in
// metres, in each body's bone frame (formats/pack-format.md, "Ragdolls in
// meshes").
#include "bethconv/mesh/gltf_writer.hpp"
#include "bethconv/mesh/nif_reader.hpp"
#include "bethconv/pack/pack_view.hpp"

#include "../support/nif_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <string>

using bethconv::mesh::ConstraintKind;
using Catch::Matchers::WithinAbs;

namespace {

constexpr float k_havok = static_cast<float>(skydot::formats::k_havok_scale);
constexpr double k_metres_per_havok = skydot::formats::k_havok_scale * skydot::formats::k_metres_per_unit;

/// Pelvis 50 units up, spine 20 above it; a capsule each, the spine held to the pelvis by a
/// ragdoll constraint, an arm (a bhkRigidBodyT offset along X) by a malleable limited hinge.
struct Skeleton {
    bethconv::test::NifBuilder builder{bethconv::test::NifFlavor::se};
    nifly::bhkRigidBody* pelvis{};
    nifly::bhkRigidBody* spine{};
    nifly::NiNode* spine_node{};

    Skeleton() {
        auto* pelvis_node = builder.add_node("NPC Pelvis");
        pelvis_node->transform.translation = nifly::Vector3(0.0f, 0.0f, 50.0f);
        spine_node = builder.add_node("NPC Spine", pelvis_node);
        spine_node->transform.translation = nifly::Vector3(0.0f, 0.0f, 20.0f);
        pelvis = builder.add_ragdoll_body(pelvis_node, {0, 0, 50}, {0, 0, 0}, {0, 0, 0.1f}, 0.2f, 10.0f, 1);
        spine = builder.add_ragdoll_body(spine_node, {0, 0, 70}, {0, 0, 0}, {0, 0, 0.2f}, 0.15f, 5.0f, 2);
        spine->linearDamping = 0.1f;
        spine->angularDamping = 0.05f;
        spine->friction = 0.3f;
        spine->restitution = 0.8f;
        spine->center = nifly::Vector4(0.0f, 0.0f, 0.1f, 0.0f);
        spine->inertiaMatrix[0] = 0.5f;
        spine->inertiaMatrix[5] = 0.25f;
        spine->inertiaMatrix[10] = 0.125f;

        auto ragdoll = std::make_unique<nifly::bhkRagdollConstraint>();
        ragdoll->ragdoll.twistA = nifly::Vector4(0, 0, 1, 0);
        ragdoll->ragdoll.planeA = nifly::Vector4(1, 0, 0, 0);
        ragdoll->ragdoll.twistB = nifly::Vector4(0, 0, 1, 0);
        ragdoll->ragdoll.planeB = nifly::Vector4(1, 0, 0, 0);
        // The spine's origin, 20 units above the pelvis, in the pelvis' frame (Havok units).
        ragdoll->ragdoll.pivotB = nifly::Vector4(0, 0, 20.0f / k_havok, 0);
        ragdoll->ragdoll.coneMaxAngle = 0.5f;
        ragdoll->ragdoll.planeMinAngle = -0.25f;
        ragdoll->ragdoll.planeMaxAngle = 0.25f;
        ragdoll->ragdoll.twistMinAngle = -0.1f;
        ragdoll->ragdoll.twistMaxAngle = 0.1f;
        ragdoll->ragdoll.maxFriction = 10.0f;
        builder.add_constraint(std::move(ragdoll), spine, pelvis);
    }
};

nlohmann::json ragdoll_json(const bethconv::mesh::Model& model) {
    const auto glb = bethconv::mesh::write_glb(model, {});
    REQUIRE(glb.has_value());
    const auto chunks = bethconv::pack::glb_chunks(*glb, "test.glb");
    REQUIRE(chunks.has_value());
    const auto doc = nlohmann::json::parse(chunks->json);
    for (const auto& node : doc["nodes"]) {
        if (node.contains("extras") && node["extras"]["bethconv"].contains("ragdoll")) {
            return node["extras"]["bethconv"]["ragdoll"];
        }
    }
    return nullptr;
}

bool warned(const bethconv::mesh::Model& model, std::string_view text) {
    return std::ranges::any_of(model.warnings, [&](const std::string& w) { return w.find(text) != std::string::npos; });
}

} // namespace

TEST_CASE("a skeleton's rigid bodies and constraints become its ragdoll", "[mesh][nif][ragdoll]") {
    Skeleton s;
    auto model = bethconv::mesh::read_nif(s.builder.bytes(), "skeleton.nif");
    REQUIRE(model.has_value());
    CHECK_FALSE(warned(*model, "ragdoll"));
    const auto& ragdoll = model->ragdoll;
    REQUIRE(ragdoll.bodies.size() == 2);
    REQUIRE(ragdoll.constraints.size() == 1);

    const auto& spine = ragdoll.bodies[1];
    CHECK(spine.bone == "NPC Spine");
    CHECK(spine.collision_object == "bhkBlendCollisionObject");
    CHECK(spine.block_name == "bhkRigidBody");
    CHECK(spine.mass == 5.0f);
    CHECK(spine.layer == 8);
    CHECK(spine.flags_and_part == 2);
    CHECK(spine.motion_type == 4);
    CHECK_THAT(spine.bone_bind.translation.z, WithinAbs(70.0, 1e-4));
    REQUIRE(spine.shapes.size() == 1);
    CHECK(spine.shapes[0].kind == bethconv::mesh::CollisionKind::capsule);
    REQUIRE(spine.constraints.size() == 1);
    CHECK(spine.constraints[0] == 0);
    CHECK(ragdoll.bodies[0].constraints.empty());

    const auto& c = ragdoll.constraints[0];
    CHECK(c.kind == ConstraintKind::ragdoll);
    CHECK(c.body_a == 1); // the child
    CHECK(c.body_b == 0);
    CHECK(c.cone_max == 0.5f);
    CHECK(c.twist_min == -0.1f);
    CHECK(c.max_friction == 10.0f);
    // The collision extras keep the same bodies.
    CHECK(model->collision.size() == 2);
}

TEST_CASE("the ragdoll extras are in metres, in each body's bone frame", "[mesh][gltf][ragdoll]") {
    Skeleton s;
    auto model = bethconv::mesh::read_nif(s.builder.bytes(), "skeleton.nif");
    REQUIRE(model.has_value());
    const auto r = ragdoll_json(*model);
    REQUIRE(r.is_object());
    REQUIRE(r["bodies"].size() == 2);
    const auto& spine = r["bodies"][1];
    CHECK(spine["bone"] == "NPC Spine");
    CHECK(spine["filter"]["layer"] == 8);
    CHECK(spine["filter"]["flags_and_part"] == 2);
    CHECK(spine["constraints"] == nlohmann::json::array({0}));
    CHECK_THAT(spine["bone_bind"]["translation"][2].get<double>(), WithinAbs(70.0 * 0.0142875, 1e-5));
    CHECK_THAT(spine["shapes"][0]["point_b"][2].get<double>(), WithinAbs(0.2 * k_metres_per_havok, 1e-6));
    CHECK_THAT(spine["shapes"][0]["radius"].get<double>(), WithinAbs(0.15 * k_metres_per_havok, 1e-6));
    CHECK_THAT(spine["center_of_mass"][2].get<double>(), WithinAbs(0.1 * k_metres_per_havok, 1e-6));
    CHECK_THAT(spine["inertia"][4].get<double>(), WithinAbs(0.25 * k_metres_per_havok * k_metres_per_havok, 1e-6));
    CHECK_THAT(spine["friction"].get<double>(), WithinAbs(0.3, 1e-6));

    const auto& c = r["constraints"][0];
    CHECK(c["type"] == "ragdoll");
    CHECK(c["body_a"] == 1);
    CHECK(c["body_b"] == 0);
    CHECK(c["in_a"]["twist"] == nlohmann::json::array({0.0, 0.0, 1.0}));
    CHECK_THAT(c["in_b"]["pivot"][2].get<double>(), WithinAbs(20.0 * 0.0142875, 1e-5));
    CHECK_THAT(c["cone_max"].get<double>(), WithinAbs(0.5, 1e-6));
    CHECK_THAT(c["plane_min"].get<double>(), WithinAbs(-0.25, 1e-6));
    CHECK(c["motor"]["type"] == "none");
    CHECK_FALSE(c.contains("wrapper"));
}

TEST_CASE("a wrapped constraint keeps its wrapper and is read as the one it wraps", "[mesh][nif][ragdoll]") {
    Skeleton s;
    auto* arm_node = s.builder.add_node("NPC Arm", s.spine_node);
    // A bhkRigidBodyT 5 Havok units along X of its bone.
    auto* t = s.builder.add_ragdoll_body(arm_node, {0, 0, 70}, {0, 0, 0}, {0.1f, 0, 0}, 0.05f, 2.0f, 3,
                                         nifly::Vector3(5.0f, 0.0f, 0.0f));
    auto malleable = std::make_unique<nifly::bhkMalleableConstraint>();
    malleable->subConstraint.type = nifly::LimitedHinge;
    malleable->subConstraint.desc3.axleA = nifly::Vector4(1, 0, 0, 0);
    malleable->subConstraint.desc3.minAngle = -1.0f;
    malleable->subConstraint.desc3.maxAngle = 0.5f;
    malleable->subConstraint.desc3.maxFriction = 0.01f;
    malleable->subConstraint.strength = 0.75f;
    malleable->subConstraint.entityRefs.SetKeepEmptyRefs();
    malleable->subConstraint.entityRefs.AddBlockRef(s.builder.file().GetBlockID(t));
    malleable->subConstraint.entityRefs.AddBlockRef(s.builder.file().GetBlockID(s.spine));
    s.builder.add_constraint(std::move(malleable), t, s.spine);

    auto model = bethconv::mesh::read_nif(s.builder.bytes(), "skeleton.nif");
    REQUIRE(model.has_value());
    CHECK_FALSE(warned(*model, "ragdoll"));
    REQUIRE(model->ragdoll.constraints.size() == 2);
    const auto& c = model->ragdoll.constraints[1];
    CHECK(c.kind == ConstraintKind::limited_hinge);
    CHECK(c.wrapper == "malleable");
    CHECK(c.block_name == "bhkMalleableConstraint");
    CHECK(c.strength == 0.75f);
    CHECK(c.min_angle == -1.0f);
    CHECK(c.body_a == 2);
    CHECK(c.body_b == 1);
    CHECK(model->ragdoll.bodies[2].block_name == "bhkRigidBodyT");

    const auto r = ragdoll_json(*model);
    const auto& arm_json = r["bodies"][2];
    CHECK_THAT(arm_json["body_transform"]["translation"][0].get<double>(), WithinAbs(5.0 * k_metres_per_havok, 1e-5));
    const auto& hinge = r["constraints"][1];
    CHECK(hinge["type"] == "limited_hinge");
    CHECK(hinge["wrapper"] == "malleable");
    CHECK_THAT(hinge["strength"].get<double>(), WithinAbs(0.75, 1e-6));
    CHECK_THAT(hinge["min_angle"].get<double>(), WithinAbs(-1.0, 1e-6));
    // Pivot A is the body's origin, which lies 5 Havok units along the bone's X.
    CHECK_THAT(hinge["in_a"]["pivot"][0].get<double>(), WithinAbs(5.0 * k_metres_per_havok, 1e-5));
    CHECK(hinge["in_a"]["axle"] == nlohmann::json::array({1.0, 0.0, 0.0}));
    // The capsule's end moves with the body.
    CHECK_THAT(arm_json["shapes"][0]["transform"]["translation"][0].get<double>(),
               WithinAbs(5.0 * k_metres_per_havok, 1e-5));
}

TEST_CASE("a NIF without constraints or blend bodies has no ragdoll", "[mesh][nif][ragdoll]") {
    bethconv::test::NifBuilder builder(bethconv::test::NifFlavor::se);
    builder.add_shape("cube", bethconv::test::make_cube());
    auto box = std::make_unique<nifly::bhkBoxShape>();
    box->dimensions = nifly::Vector3(1.0f, 1.0f, 1.0f);
    builder.add_collision(std::move(box), {});
    auto model = bethconv::mesh::read_nif(builder.bytes(), "rock.nif");
    REQUIRE(model.has_value());
    CHECK(model->collision.size() == 1);
    CHECK(model->ragdoll.bodies.empty());
    const auto glb = bethconv::mesh::write_glb(*model, {});
    REQUIRE(glb.has_value());
    const auto chunks = bethconv::pack::glb_chunks(*glb, "rock.glb");
    REQUIRE(chunks.has_value());
    CHECK(chunks->json.find("\"ragdoll\"") == std::string_view::npos);
}

TEST_CASE("a ragdoll body away from its bone, or a constraint that cannot be read, is reported",
          "[mesh][nif][ragdoll]") {
    Skeleton s;
    SECTION("a bhkRigidBody that does not store its bone's bind transform") {
        s.spine->translation = nifly::Vector4(0.0f, 0.0f, 90.0f / k_havok, 0.0f);
        auto model = bethconv::mesh::read_nif(s.builder.bytes(), "skeleton.nif");
        REQUIRE(model.has_value());
        CHECK(warned(*model, "ragdoll body on 'NPC Spine': stored transform is not the node's at bind"));
        CHECK(model->ragdoll.bodies.size() == 2);
    }
    SECTION("a constraint to a rigid body on no node") {
        auto& header = s.builder.file().GetHeader();
        auto loose = std::make_unique<nifly::bhkRigidBody>();
        auto* raw = loose.get();
        header.AddBlock(std::move(loose));
        s.builder.add_constraint(std::make_unique<nifly::bhkBallAndSocketConstraint>(), s.pelvis, raw);
        auto model = bethconv::mesh::read_nif(s.builder.bytes(), "skeleton.nif");
        REQUIRE(model.has_value());
        CHECK(warned(*model, "bhkBallAndSocketConstraint joins a body that is not on a node"));
        CHECK(model->ragdoll.constraints.size() == 1);
    }
    SECTION("a body without a shape is kept, with none") {
        s.spine->shapeRef.index = nifly::NIF_NPOS;
        auto model = bethconv::mesh::read_nif(s.builder.bytes(), "skeleton.nif");
        REQUIRE(model.has_value());
        REQUIRE(model->ragdoll.bodies.size() == 2);
        CHECK(model->ragdoll.bodies[1].shapes.empty());
        CHECK(model->ragdoll.bodies[0].shapes.size() == 1);
    }
    SECTION("a constraint reference to a block that is no constraint") {
        auto& header = s.builder.file().GetHeader();
        s.pelvis->constraintRefs.AddBlockRef(header.AddBlock(std::make_unique<nifly::bhkSphereShape>()));
        auto model = bethconv::mesh::read_nif(s.builder.bytes(), "skeleton.nif");
        REQUIRE(model.has_value());
        CHECK(warned(*model, "is not one that is read (none)"));
        CHECK(model->ragdoll.constraints.size() == 1);
    }
}

TEST_CASE("a rigid body shared by two nodes does not shift the bodies after it", "[mesh][nif][ragdoll]") {
    Skeleton s;
    // A second node on the spine's collision object: the spine's body is met twice.
    auto* copy = s.builder.add_node("NPC Spine Copy", s.spine_node);
    copy->collisionRef.index = s.spine_node->collisionRef.index;
    auto* arm_node = s.builder.add_node("NPC Arm", s.spine_node);
    arm_node->transform.translation = nifly::Vector3(10.0f, 0.0f, 0.0f);
    auto* arm = s.builder.add_ragdoll_body(arm_node, {10, 0, 70}, {0, 0, 0}, {0.1f, 0, 0}, 0.05f, 2.0f, 3);
    s.builder.add_constraint(std::make_unique<nifly::bhkRagdollConstraint>(), arm, s.spine);

    auto model = bethconv::mesh::read_nif(s.builder.bytes(), "skeleton.nif");
    REQUIRE(model.has_value());
    const auto& ragdoll = model->ragdoll;
    REQUIRE(ragdoll.bodies.size() == 4);
    CHECK(ragdoll.bodies[3].bone == "NPC Arm");
    REQUIRE(ragdoll.constraints.size() == 2);
    CHECK(ragdoll.constraints[1].body_a == 3);
    CHECK(ragdoll.constraints[1].body_b == 1);
}
