// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotAnimator`: plays the clips the converter writes into a scene's root
// extras (`bethconv.animations`): node transforms and visibility, shader
// variables (UV scrolling, emissive colour and strength, alpha, falloff),
// alpha-test thresholds and particle emission rates.
//
// Unnamed clips come from controllers that run on their own in the game; they
// follow the application clock, so every copy of a model flickers in step, as
// in the game. Named clips come from NiControllerSequence (`Open`, `Close`,
// `Idle`) and play on request; `Idle`-like ones start by themselves.
//
// Shader variables are written to a per-instance copy of the material, made
// the first time a clip touches it.
#pragma once

#include "render/materials.hpp"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace skydot {

struct EffectAsset;
class SkydotParticles;

class SkydotAnimator : public godot::Node {
    GDCLASS(SkydotAnimator, godot::Node)

public:
    /// Set up what the converter's extras describe under `root` (an
    /// instantiated model): hide nodes the NIF hides, add an animator for its
    /// clips and a SkydotParticles per particle system. `materials` converts
    /// particle materials. Returns the number of clips plus particle systems.
    static std::int64_t attach(godot::Node* root, const godot::Ref<SkydotMaterials>& materials);

    /// Start a named clip from its beginning; unnamed clips keep running.
    /// Returns false if there is no such clip.
    bool play(const godot::String& clip);
    void stop();
    godot::String get_playing() const;
    godot::PackedStringArray get_clip_names() const;
    std::int64_t get_channel_count() const;

    /// Pose everything as at application time `seconds`, with the named clip
    /// `seconds_into_clip` in. Used by tests and by _process.
    void evaluate(double seconds, double seconds_into_clip);

    void _ready() override;
    void _process(double delta) override;

protected:
    static void _bind_methods();

private:
    struct Target {
        godot::Node3D* node = nullptr;
        // A skinned model's animated nodes are bones of a Skeleton3D.
        godot::Skeleton3D* skeleton = nullptr;
        std::int32_t bone = -1;
        godot::Transform3D rest;
        godot::Quaternion rest_rotation;
        godot::Vector3 rest_scale;
        // Pose accumulated this frame.
        bool has_translation = false, has_rotation = false, has_euler = false,
             has_scale = false;
        godot::Vector3 translation;
        godot::Quaternion rotation;
        godot::Vector3 euler;
        float scale = 1.0f;
        // Material state, bound on first use.
        bool material_bound = false;
        bool effect = false;
        bool shared = false; ///< Material shared by every instance.
        godot::Ref<godot::ShaderMaterial> material;
        godot::Vector2 uv_offset, uv_scale;
        godot::Color emission;
        float emission_strength = 1.0f;
        godot::Vector4 falloff;
        godot::Color base_color;
        float glossiness = 0.0f, specular_strength = 0.0f, env_scale = 1.0f;
        godot::Vector3 specular_color;
        float alpha_cutoff = 0.5f;
        std::uint32_t dirty = 0;
        SkydotParticles* particles = nullptr;
        bool particles_bound = false;
    };

    std::shared_ptr<const EffectAsset> asset_;
    std::vector<Target> targets_;
    int playing_ = -1;
    double clip_time_ = 0.0;
    double last_local_ = -1.0; ///< Clip time last evaluated; -1 before the first.
    bool finished_ = false;

    void bind(const std::shared_ptr<const EffectAsset>& asset, godot::Node* root);
    void bind_material(Target& target, std::size_t slot);
    void apply_clip(std::size_t clip, double local_time);
    void flush();
    static godot::Transform3D pose_of(const Target& t);
};

} // namespace skydot
