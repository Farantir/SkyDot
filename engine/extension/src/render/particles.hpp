// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotParticles`: one NiParticleSystem, built from the block the converter
// writes into the model's extras. Each emitter becomes a GPUParticles3D whose
// process shader follows Gamebryo's modifiers: box, sphere, cylinder and mesh
// emitters, declination and planar angle, planar and spherical gravity with
// decay and turbulence, drag, spin, the scale curve, grow/fade, the three-stop
// colour modifier with fade in/out, colour keys and sub-texture animation.
// Particles are drawn as camera-facing quads with the effect shader.
//
// Built on entering the tree, because emitters, gravity objects and the
// particles' own space depend on global transforms.
#pragma once

#include "render/materials.hpp"

#include <godot_cpp/classes/gpu_particles3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace skydot {

class SkydotParticles : public godot::Node3D {
    GDCLASS(SkydotParticles, godot::Node3D)

public:
    /// `block` is one entry of the converter's `particles`; `nodes` maps its
    /// node ids to the instantiated model's nodes.
    void setup(const godot::Dictionary& block, const godot::Ref<godot::ShaderMaterial>& draw,
               const godot::Ref<SkydotMaterials>& materials,
               const std::unordered_map<std::int64_t, godot::Node*>& nodes);

    /// Emission as a fraction of the peak rate the converter found, from the
    /// `particles.birth_rate` channel (particles per second).
    void set_birth_rate(double rate);
    void set_active(bool active);
    std::int64_t get_emitter_count() const;
    /// Particles per second each emitter starts with.
    double get_birth_rate() const;

    void _ready() override;

protected:
    static void _bind_methods();

private:
    struct Emitter {
        godot::Dictionary block;
        godot::Node3D* node = nullptr;
        std::vector<godot::Node3D*> meshes;
        godot::GPUParticles3D* particles = nullptr;
        double peak_rate = 0.0;
    };

    godot::Dictionary block_;
    godot::Ref<godot::ShaderMaterial> draw_;
    godot::Ref<SkydotMaterials> materials_;
    std::vector<Emitter> emitters_;
    /// A gravity or drag modifier and the node it is placed by.
    struct Field {
        godot::Dictionary block;
        godot::Node3D* node = nullptr;
    };
    std::vector<Field> gravity_;
    std::vector<Field> drags_;
    double rate_ = -1.0;
    bool active_ = true;
    bool built_ = false;

    void build();
    void build_emitter(Emitter& emitter);
    void update_emission();
};

} // namespace skydot
