// SPDX-License-Identifier: GPL-3.0-or-later
//
// Controllers, sequences and particle systems, written with nifly and read
// back into Model::animations and Model::particles.
#include "bethconv/mesh/gltf_writer.hpp"
#include "bethconv/mesh/nif_reader.hpp"

#include "../support/nif_builder.hpp"

#include <Animation.hpp>
#include <Particles.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <fastgltf/core.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

using bethconv::mesh::AnimationChannel;
using bethconv::mesh::AnimationClip;
using bethconv::mesh::CycleMode;
using bethconv::mesh::EmitterKind;
using bethconv::mesh::KeyInterp;
using bethconv::mesh::Model;
using bethconv::test::NifBuilder;

namespace {

/// NiTimeController flags: bit 3 active, bits 1-2 cycle type.
constexpr std::uint16_t k_active = 0x0008;
constexpr std::uint16_t k_clamp = 0x0004;

template <typename T>
std::pair<T*, std::uint32_t> add(nifly::NifFile& nif, std::unique_ptr<T> block) {
    T* raw = block.get();
    const std::uint32_t index = nif.GetHeader().AddBlock(std::move(block));
    return {raw, index};
}

const AnimationChannel* find(const AnimationClip& clip, const Model& model,
                             std::string_view node, std::string_view property) {
    for (const AnimationChannel& ch : clip.channels) {
        if (model.nodes[ch.node].name == node && ch.property == property) {
            return &ch;
        }
    }
    return nullptr;
}

/// A flame shape whose effect shader scrolls U on its own, a door the "Open"
/// sequence turns, and a smoke particle system with a box emitter and
/// gravity.
Model build(bool hide_flame = false) {
    NifBuilder builder(bethconv::test::NifFlavor::se);
    nifly::NifFile& nif = builder.file();

    auto* flame = builder.add_shape("flame", bethconv::test::make_cube());
    if (hide_flame) {
        flame->flags |= 1u;
    }
    auto [shader, shader_index] = add(nif, std::make_unique<nifly::BSEffectShaderProperty>());
    shader->sourceTexture.get() = "textures/effects/flame.dds";
    flame->ShaderPropertyRef()->index = shader_index;

    auto [data, data_index] = add(nif, std::make_unique<nifly::NiFloatData>());
    data->data.SetInterpolationType(nifly::LINEAR_KEY);
    nifly::NiAnimationKey<float> key;
    key.time = 0.0f;
    key.value = 0.0f;
    data->data.AddKey(key);
    key.time = 2.0f;
    key.value = 1.0f;
    data->data.AddKey(key);
    auto [interp, interp_index] = add(nif, std::make_unique<nifly::NiFloatInterpolator>());
    interp->dataRef.index = data_index;
    auto [scroll, scroll_index] =
        add(nif, std::make_unique<nifly::BSEffectShaderPropertyFloatController>());
    scroll->typeOfControlledVariable = 6; // U offset
    scroll->flags = k_active;
    scroll->frequency = 1.0f;
    scroll->startTime = 0.0f;
    scroll->stopTime = 2.0f;
    scroll->interpolatorRef.index = interp_index;
    scroll->targetRef.index = shader_index;
    shader->controllerRef.index = scroll_index;

    // The door and its sequence.
    auto* door = builder.add_node("Door");
    auto [tdata, tdata_index] = add(nif, std::make_unique<nifly::NiTransformData>());
    tdata->rotationType = nifly::XYZ_ROTATION_KEY;
    for (auto* axis : {&tdata->xRotations, &tdata->yRotations, &tdata->zRotations}) {
        axis->SetInterpolationType(nifly::LINEAR_KEY);
        nifly::NiAnimationKey<float> k;
        k.time = 0.0f;
        k.value = 0.0f;
        axis->AddKey(k);
        k.time = 1.0f;
        k.value = axis == &tdata->zRotations ? -1.5f : 0.0f;
        axis->AddKey(k);
    }
    auto [tinterp, tinterp_index] = add(nif, std::make_unique<nifly::NiTransformInterpolator>());
    tinterp->dataRef.index = tdata_index;
    tinterp->translation = nifly::Vector3(3.0f, 0.0f, 0.0f);
    tinterp->scale = 1.0f;
    auto [multi, multi_index] =
        add(nif, std::make_unique<nifly::NiMultiTargetTransformController>());
    multi->flags = k_active;
    auto [sequence, sequence_index] = add(nif, std::make_unique<nifly::NiControllerSequence>());
    sequence->name.get() = "Open";
    sequence->cycleType = nifly::CYCLE_CLAMP;
    sequence->frequency = 1.0f;
    sequence->startTime = 0.0f;
    sequence->stopTime = 1.0f;
    nifly::ControllerLink link;
    link.interpolatorRef.index = tinterp_index;
    link.controllerRef.index = multi_index;
    link.nodeName.get() = "Door";
    link.ctrlType.get() = "NiTransformController";
    sequence->controlledBlocks.push_back(link);
    auto [manager, manager_index] = add(nif, std::make_unique<nifly::NiControllerManager>());
    manager->flags = k_active | k_clamp;
    manager->controllerSequenceRefs.AddBlockRef(sequence_index);
    manager->nextControllerRef.index = multi_index;
    nif.GetRootNode()->controllerRef.index = manager_index;
    (void)door;

    // Smoke.
    auto* up = builder.add_node("Up");
    up->transform.rotation = nifly::Matrix3(); // identity
    auto [psys_data, psys_data_index] = add(nif, std::make_unique<nifly::NiPSysData>());
    (void)psys_data;
    auto [box, box_index] = add(nif, std::make_unique<nifly::NiPSysBoxEmitter>());
    box->name.get() = "Box";
    box->width = 10.0f;
    box->height = 20.0f;
    box->depth = 30.0f;
    box->speed = 60.0f;
    box->lifeSpan = 2.0f;
    box->radius = 8.0f;
    box->emitterNodeRef.index = nif.GetBlockID(up);
    auto [gravity, gravity_index] = add(nif, std::make_unique<nifly::NiPSysGravityModifier>());
    gravity->name.get() = "Gravity";
    gravity->gravityAxis = nifly::Vector3(0.0f, 0.0f, 1.0f);
    gravity->strength = 72.0f;
    gravity->forceType = nifly::FORCE_PLANAR;
    gravity->gravityObjRef.index = nif.GetBlockID(up);
    auto [bomb, bomb_index] = add(nif, std::make_unique<nifly::NiPSysBombModifier>());
    bomb->name.get() = "Bomb";
    auto [smoke_shader, smoke_shader_index] =
        add(nif, std::make_unique<nifly::BSEffectShaderProperty>());
    smoke_shader->sourceTexture.get() = "textures/effects/smoke.dds";
    auto [rate, rate_index] = add(nif, std::make_unique<nifly::NiFloatInterpolator>());
    rate->floatValue = 12.0f;
    auto [emit, emit_index] = add(nif, std::make_unique<nifly::NiPSysEmitterCtlr>());
    emit->flags = k_active;
    emit->frequency = 1.0f;
    emit->startTime = 0.0f;
    emit->stopTime = 1.0f;
    emit->modifierName.get() = "Box";
    emit->interpolatorRef.index = rate_index;
    auto* smoke = builder.add_node<nifly::NiParticleSystem>("smoke");
    smoke->dataRef.index = psys_data_index;
    smoke->shaderPropertyRef.index = smoke_shader_index;
    smoke->isWorldSpace = true;
    smoke->modifierRefs.AddBlockRef(box_index);
    smoke->modifierRefs.AddBlockRef(gravity_index);
    smoke->modifierRefs.AddBlockRef(bomb_index);
    smoke->controllerRef.index = emit_index;

    const auto bytes = builder.bytes();
    REQUIRE_FALSE(bytes.empty());
    auto model = bethconv::mesh::read_nif(bytes, "effects.nif");
    REQUIRE(model.has_value());
    return std::move(*model);
}

} // namespace

TEST_CASE("a controller outside a manager becomes an autoplaying clip", "[mesh][animation]") {
    const Model model = build();
    const auto clip = std::ranges::find_if(model.animations, [&](const AnimationClip& c) {
        return c.name.empty() && find(c, model, "flame", "effect.u_offset") != nullptr;
    });
    REQUIRE(clip != model.animations.end());
    CHECK(clip->autoplay);
    CHECK(clip->cycle == CycleMode::loop);
    CHECK(clip->stop == 2.0f);
    const AnimationChannel* ch = find(*clip, model, "flame", "effect.u_offset");
    CHECK(ch->interp == KeyInterp::linear);
    CHECK(ch->times == std::vector<float>{0.0f, 2.0f});
    CHECK(ch->values == std::vector<float>{0.0f, 1.0f});
    CHECK(model.nodes[ch->node].referenced);
}

TEST_CASE("a controller sequence becomes a named clip that waits to be played",
          "[mesh][animation]") {
    const Model model = build();
    const auto open = std::ranges::find_if(model.animations,
                                           [](const AnimationClip& c) { return c.name == "Open"; });
    REQUIRE(open != model.animations.end());
    CHECK_FALSE(open->autoplay);
    CHECK(open->cycle == CycleMode::clamp);
    const AnimationChannel* z = find(*open, model, "Door", "rotation_z");
    REQUIRE(z != nullptr);
    CHECK(z->values.back() == -1.5f);
    // Translation has no keys, so the interpolator's pose holds.
    const AnimationChannel* t = find(*open, model, "Door", "translation");
    REQUIRE(t != nullptr);
    CHECK(t->interp == KeyInterp::step);
    CHECK(t->values == std::vector<float>{3.0f, 0.0f, 0.0f});
}

TEST_CASE("a particle system keeps its emitter, gravity and material", "[mesh][particles]") {
    const Model model = build();
    REQUIRE(model.particles.size() == 1);
    const auto& ps = model.particles.front();
    CHECK(model.nodes[ps.node].name == "smoke");
    CHECK(ps.world_space);
    REQUIRE(ps.emitters.size() == 1);
    const auto& em = ps.emitters.front();
    CHECK(em.kind == EmitterKind::box);
    CHECK(em.size.x == 10.0f);
    CHECK(em.size.z == 30.0f);
    CHECK(em.speed == 60.0f);
    CHECK(em.birth_rate == 12.0f); // from the emitter controller's constant
    REQUIRE(em.node.has_value());
    CHECK(model.nodes[*em.node].name == "Up");
    REQUIRE(ps.gravity.size() == 1);
    CHECK(ps.gravity.front().strength == 72.0f);
    CHECK_FALSE(ps.gravity.front().spherical);
    CHECK(ps.unsupported == std::vector<std::string>{"NiPSysBombModifier"});
    CHECK(model.materials[ps.material].textures[0] == "textures/effects/smoke.dds");
    CHECK(std::ranges::any_of(model.warnings, [](const std::string& w) {
        return w.find("NiPSysBombModifier") != std::string::npos;
    }));
}

TEST_CASE("the hidden flag is kept", "[mesh][animation]") {
    const Model shown = build(false);
    const Model hidden = build(true);
    auto flame = [](const Model& m) {
        return std::ranges::find_if(m.nodes, [](const auto& n) { return n.name == "flame"; });
    };
    CHECK_FALSE(flame(shown)->hidden);
    CHECK(flame(hidden)->hidden);
}

TEST_CASE("reading animations can be turned off", "[mesh][animation]") {
    NifBuilder builder(bethconv::test::NifFlavor::se);
    builder.add_shape("cube", bethconv::test::make_cube());
    bethconv::mesh::ReadOptions options;
    options.read_animations = false;
    const auto model = bethconv::mesh::read_nif(builder.bytes(), "cube.nif", options);
    REQUIRE(model.has_value());
    CHECK(model->animations.empty());
    CHECK(model->particles.empty());
}

TEST_CASE("clips, particles and node ids reach the glTF extras", "[gltf][animation]") {
    const Model model = build(true);
    const auto glb = bethconv::mesh::write_glb(model);
    REQUIRE(glb.has_value());

    // The JSON chunk follows the 12-byte header and 8-byte chunk header.
    const auto* bytes = reinterpret_cast<const char*>(glb->data());
    std::uint32_t length = 0;
    std::memcpy(&length, bytes + 12, 4);
    const auto json = nlohmann::json::parse(std::string(bytes + 20, length));

    const nlohmann::json* root = nullptr;
    std::size_t with_id = 0;
    bool hidden = false;
    for (const auto& node : json["nodes"]) {
        if (!node.contains("extras")) {
            continue;
        }
        const auto& ours = node["extras"]["bethconv"];
        if (ours.contains("animations")) {
            root = &ours;
        }
        with_id += ours.contains("id") ? 1u : 0u;
        hidden = hidden || ours.value("hidden", false);
    }
    REQUIRE(root != nullptr);
    CHECK((*root)["animations"].size() == model.animations.size());
    REQUIRE((*root)["particles"].size() == 1);
    const auto& ps = (*root)["particles"][0];
    CHECK(ps["material"]["texture_slots"]["0"]["path"] == "textures/effects/smoke.dds");
    CHECK(ps["emitters"][0]["kind"] == "box");
    CHECK(hidden);
    // Every node a channel or particle system names carries its id.
    std::size_t referenced = 0;
    for (const auto& n : model.nodes) {
        referenced += n.referenced ? 1u : 0u;
    }
    CHECK(with_id == referenced);
    CHECK(referenced >= 4); // flame, door, smoke, Up
}
