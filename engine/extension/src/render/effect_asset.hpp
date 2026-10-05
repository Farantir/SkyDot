// SPDX-License-Identifier: GPL-3.0-or-later
//
// The converter's effect extras (clips, particle systems, node ids), parsed
// once per scene file and shared by every instance of it.
#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skydot {

enum class Prop : std::uint8_t {
    unknown,
    translation,
    rotation,
    rotation_x,
    rotation_y,
    rotation_z,
    scale,
    visible,
    alpha_test_ref,
    emissive_multiple,
    alpha,
    u_offset,
    v_offset,
    u_scale,
    v_scale,
    falloff_start_angle,
    falloff_stop_angle,
    falloff_start_opacity,
    falloff_stop_opacity,
    emissive_color,
    glossiness,
    specular_strength,
    specular_color,
    environment_map_scale,
    birth_rate,
    emitter_active,
};

enum class Interp : std::uint8_t { step, linear, cubic };
enum class Cycle : std::uint8_t { loop, reverse, clamp };

struct Channel {
    std::size_t target = 0; ///< Index into EffectAsset::ids.
    Prop prop = Prop::unknown;
    Interp interp = Interp::linear;
    int components = 1;
    std::vector<float> times, values, in, out;

    /// Value at `time`, `components` floats into `out_value`.
    void sample(double time, float* out_value) const;
};

struct Clip {
    godot::String name;
    bool autoplay = false;
    Cycle cycle = Cycle::loop;
    double frequency = 1.0, phase = 0.0, start = 0.0, stop = 0.0;
    std::vector<Channel> channels;
    std::vector<std::pair<double, godot::String>> text_keys;

    /// Controller time for a clock reading, per Gamebryo's cycle types.
    [[nodiscard]] double local_time(double clock) const;
};

struct EffectAsset {
    std::vector<std::int64_t> ids; ///< Node ids that channels target.
    /// Per target: a named clip drives its material, so each instance needs
    /// its own copy. Otherwise only the shared clock does, and every instance
    /// may share one.
    std::vector<bool> own_material;
    std::vector<Clip> clips;
    godot::Array particles;        ///< The converter's particle blocks.
    int autoplay_sequence = -1;    ///< Named clip started on load.
    bool has_hidden = false;       ///< Some node is hidden until shown.

    [[nodiscard]] bool empty() const {
        return clips.empty() && particles.is_empty() && !has_hidden;
    }

    /// Parse the extras of an instantiated model (see
    /// SkydotMaterials::effects, which caches this by scene file).
    static std::shared_ptr<const EffectAsset> parse(godot::Node* root);
};

/// Every node under `root` that carries a converter id, by id.
std::unordered_map<std::int64_t, godot::Node*> nodes_by_id(godot::Node* root);

/// The `bethconv` block of a node's extras, or an empty dictionary.
godot::Dictionary bethconv_extras(const godot::Node* node);

} // namespace skydot
