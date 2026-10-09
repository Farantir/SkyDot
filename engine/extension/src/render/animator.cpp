// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/animator.hpp"

#include <cmath>
#include <numbers>
#include <vector>

#include "render/billboard.hpp"
#include "render/effect_asset.hpp"
#include "render/particles.hpp"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/basis.hpp>

#include <algorithm>
#include <unordered_map>

using godot::Color;
using godot::Dictionary;
using godot::Quaternion;
using godot::Ref;
using godot::String;
using godot::Variant;
using godot::Vector2;
using godot::Vector3;
using godot::Vector4;

namespace skydot {

namespace {

/// Beyond this (metres from the camera) models hold still; particles, which
/// run on the GPU, keep going.
constexpr double k_max_distance = 250.0;

/// Shared materials already written this frame, by instance id. Every copy of
/// a model computes the same clock-driven values, so the first one to run
/// writes them for all.
std::unordered_map<std::uint64_t, std::uint64_t>& shared_writes() {
    static std::unordered_map<std::uint64_t, std::uint64_t> frames;
    return frames;
}

/// Bones carrying the converter's node ids (glTF node extras become bone
/// meta when the importer turns skinned nodes into a Skeleton3D).
void bones_by_id(godot::Node* node,
                 std::unordered_map<std::int64_t, std::pair<godot::Skeleton3D*, std::int32_t>>& out) {
    if (auto* skeleton = godot::Object::cast_to<godot::Skeleton3D>(node)) {
        for (std::int32_t b = 0; b < skeleton->get_bone_count(); ++b) {
            if (!skeleton->has_bone_meta(b, "extras")) {
                continue;
            }
            const Variant extras = skeleton->get_bone_meta(b, "extras");
            if (extras.get_type() != Variant::DICTIONARY) {
                continue;
            }
            const Variant block = Dictionary(extras).get("bethconv", Variant());
            if (block.get_type() != Variant::DICTIONARY) {
                continue;
            }
            const Variant id = Dictionary(block).get("id", Variant());
            if (id.get_type() == Variant::INT || id.get_type() == Variant::FLOAT) {
                out.try_emplace(static_cast<std::int64_t>(id), skeleton, b);
            }
        }
    }
    for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
        bones_by_id(node->get_child(i), out);
    }
}

enum Dirty : std::uint32_t {
    k_uv_offset = 1u << 0,
    k_uv_scale = 1u << 1,
    k_emission = 1u << 2,
    k_emission_strength = 1u << 3,
    k_falloff = 1u << 4,
    k_base_color = 1u << 5,
    k_glossiness = 1u << 6,
    k_specular_strength = 1u << 7,
    k_specular_color = 1u << 8,
    k_env_scale = 1u << 9,
    k_alpha_cutoff = 1u << 10,
};

/// Nodes the NIF marks hidden stay hidden, except the intact stage of a
/// destructible (BSDamageStage's DamageStage0): the game shows the stage the
/// reference is in, and every reference starts at 0. Without this a barricade
/// or siege wall was solid but invisible.
void hide_marked(godot::Node* node) {
    if (auto* n = godot::Object::cast_to<godot::Node3D>(node)) {
        if (static_cast<bool>(bethconv_extras(n).get("hidden", false)) &&
            n->get_name() != godot::StringName("DamageStage0")) {
            n->set_visible(false);
        }
    }
    for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
        hide_marked(node->get_child(i));
    }
}

godot::MeshInstance3D* mesh_of(godot::Node3D* node) {
    if (auto* m = godot::Object::cast_to<godot::MeshInstance3D>(node)) {
        return m;
    }
    for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
        if (auto* m = godot::Object::cast_to<godot::MeshInstance3D>(node->get_child(i))) {
            return m;
        }
    }
    return nullptr;
}

template <typename T>
T param(const Ref<godot::ShaderMaterial>& m, const char* name, T fallback) {
    const Variant v = m->get_shader_parameter(name);
    return v.get_type() == Variant(fallback).get_type() ? static_cast<T>(v) : fallback;
}

/// Colours are stored as vectors (see shader_rgba in materials.hpp).
godot::Color param_color(const Ref<godot::ShaderMaterial>& m, const char* name, godot::Color fallback) {
    const Variant v = m->get_shader_parameter(name);
    if (v.get_type() == Variant::VECTOR4) {
        const Vector4 c = v;
        return {c.x, c.y, c.z, c.w};
    }
    if (v.get_type() == Variant::VECTOR3) {
        const Vector3 c = v;
        return {c.x, c.y, c.z, fallback.a};
    }
    return v.get_type() == Variant::COLOR ? static_cast<godot::Color>(v) : fallback;
}

float param_float(const Ref<godot::ShaderMaterial>& m, const char* name, float fallback) {
    const Variant v = m->get_shader_parameter(name);
    return v.get_type() == Variant::FLOAT || v.get_type() == Variant::INT
               ? static_cast<float>(static_cast<double>(v))
               : fallback;
}

} // namespace

void SkydotAnimator::_bind_methods() {
    using godot::D_METHOD;
    godot::ClassDB::bind_static_method("SkydotAnimator", D_METHOD("attach", "root", "materials"),
                                       &SkydotAnimator::attach);
    godot::ClassDB::bind_method(D_METHOD("play", "clip"), &SkydotAnimator::play);
    godot::ClassDB::bind_method(D_METHOD("stop"), &SkydotAnimator::stop);
    godot::ClassDB::bind_method(D_METHOD("get_playing"), &SkydotAnimator::get_playing);
    godot::ClassDB::bind_method(D_METHOD("get_clip_names"), &SkydotAnimator::get_clip_names);
    godot::ClassDB::bind_method(D_METHOD("get_channel_count"), &SkydotAnimator::get_channel_count);
    godot::ClassDB::bind_method(D_METHOD("evaluate", "seconds", "seconds_into_clip"),
                                &SkydotAnimator::evaluate);
    ADD_SIGNAL(godot::MethodInfo("text_key", godot::PropertyInfo(Variant::STRING, "clip"),
                                 godot::PropertyInfo(Variant::STRING, "key")));
    ADD_SIGNAL(godot::MethodInfo("finished", godot::PropertyInfo(Variant::STRING, "clip")));
}

std::int64_t SkydotAnimator::attach(godot::Node* root, const Ref<SkydotMaterials>& materials) {
    if (root == nullptr || materials.is_null()) {
        return 0;
    }
    const std::shared_ptr<const EffectAsset> asset = materials->effects(root);
    if (asset->empty()) {
        return 0;
    }
    if (asset->has_hidden) {
        hide_marked(root);
    }
    if (asset->clips.empty() && asset->particles.is_empty()) {
        return 0;
    }

    const auto nodes = nodes_by_id(root);
    std::int64_t count = 0;
    const String scene = root->get_scene_file_path();
    for (std::int64_t i = 0; i < asset->particles.size(); ++i) {
        if (asset->particles[i].get_type() != Variant::DICTIONARY) {
            continue;
        }
        const Dictionary block = asset->particles[i];
        const auto it = nodes.find(static_cast<std::int64_t>(block.get("node", -1)));
        auto* target = it != nodes.end() ? godot::Object::cast_to<godot::Node3D>(it->second) : nullptr;
        if (target == nullptr) {
            continue;
        }
        auto* particles = memnew(SkydotParticles);
        particles->set_name("SkydotParticles");
        particles->setup(block, materials->particle_material_for(scene, i, block), materials, nodes);
        target->add_child(particles);
        ++count;
    }

    if (!asset->clips.empty()) {
        auto* animator = memnew(SkydotAnimator);
        animator->set_name("SkydotAnimator");
        animator->bind(asset, root);
        root->add_child(animator);
        count += static_cast<std::int64_t>(asset->clips.size());
    }
    return count;
}

void SkydotAnimator::bind(const std::shared_ptr<const EffectAsset>& asset, godot::Node* root) {
    asset_ = asset;
    const auto nodes = nodes_by_id(root);
    std::unordered_map<std::int64_t, std::pair<godot::Skeleton3D*, std::int32_t>> bones;
    bones_by_id(root, bones);
    targets_.assign(asset->ids.size(), Target{});
    for (std::size_t i = 0; i < asset->ids.size(); ++i) {
        const auto it = nodes.find(asset->ids[i]);
        auto* node = it != nodes.end() ? godot::Object::cast_to<godot::Node3D>(it->second) : nullptr;
        Target& t = targets_[i];
        t.node = node;
        if (node != nullptr) {
            t.rest = node->get_transform();
        } else if (const auto b = bones.find(asset->ids[i]); b != bones.end()) {
            t.skeleton = b->second.first;
            t.bone = b->second.second;
            t.rest = t.skeleton->get_bone_pose(t.bone);
        } else {
            continue;
        }
        t.rest_rotation = t.rest.basis.get_rotation_quaternion();
        t.rest_scale = t.rest.basis.get_scale();
    }
}

void SkydotAnimator::_ready() {
    // An emitter that only named clips drive (the forge's hammer sparks, the
    // sharpening wheel's) waits for one to play, as the game's furniture
    // does until it is used; unnamed clips drive theirs all the time.
    if (asset_ != nullptr) {
        std::vector<char> named(targets_.size(), 0);
        for (const Clip& clip : asset_->clips) {
            const bool always = clip.name.is_empty() && clip.autoplay;
            for (const Channel& ch : clip.channels) {
                if (ch.prop == Prop::birth_rate || ch.prop == Prop::emitter_active) {
                    char& state = named[ch.target];
                    state = always ? 2 : (state == 0 ? 1 : state);
                }
            }
        }
        for (std::size_t i = 0; i < targets_.size(); ++i) {
            if (named[i] != 1 || targets_[i].node == nullptr) {
                continue;
            }
            for (std::int32_t c = 0; c < targets_[i].node->get_child_count(); ++c) {
                if (auto* p = godot::Object::cast_to<SkydotParticles>(targets_[i].node->get_child(c))) {
                    p->set_active(false);
                }
            }
        }
    }
    if (asset_ != nullptr && asset_->autoplay_sequence >= 0) {
        play(asset_->clips[static_cast<std::size_t>(asset_->autoplay_sequence)].name);
    }
    set_process(true);
}

void SkydotAnimator::_process(double delta) {
    if (playing_ >= 0) {
        clip_time_ += delta;
    }
    const auto* root = godot::Object::cast_to<godot::Node3D>(get_parent());
    const godot::Viewport* viewport = get_viewport();
    const godot::Camera3D* camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
    if (root != nullptr && camera != nullptr &&
        static_cast<double>(root->get_global_position().distance_to(
            camera->get_global_position())) > k_max_distance) {
        return;
    }
    evaluate(static_cast<double>(godot::Time::get_singleton()->get_ticks_usec()) / 1e6, clip_time_);
}

bool SkydotAnimator::play(const String& clip) {
    if (asset_ == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < asset_->clips.size(); ++i) {
        if (!clip.is_empty() && asset_->clips[i].name == clip) {
            playing_ = static_cast<int>(i);
            clip_time_ = 0.0;
            finished_ = false;
            last_local_ = asset_->clips[i].start - 1e-6;
            return true;
        }
    }
    return false;
}

void SkydotAnimator::stop() { playing_ = -1; }

String SkydotAnimator::get_playing() const {
    return playing_ >= 0 ? asset_->clips[static_cast<std::size_t>(playing_)].name : String();
}

godot::PackedStringArray SkydotAnimator::get_clip_names() const {
    godot::PackedStringArray out;
    if (asset_ != nullptr) {
        for (const Clip& clip : asset_->clips) {
            if (!clip.name.is_empty()) {
                out.push_back(clip.name);
            }
        }
    }
    return out;
}

std::int64_t SkydotAnimator::get_channel_count() const {
    std::int64_t count = 0;
    if (asset_ != nullptr) {
        for (const Clip& clip : asset_->clips) {
            count += static_cast<std::int64_t>(clip.channels.size());
        }
    }
    return count;
}

void SkydotAnimator::evaluate(double seconds, double seconds_into_clip) {
    if (asset_ == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < asset_->clips.size(); ++i) {
        const Clip& clip = asset_->clips[i];
        if (clip.name.is_empty() && clip.autoplay) {
            apply_clip(i, clip.local_time(seconds));
        }
    }
    if (playing_ >= 0) {
        const Clip& clip = asset_->clips[static_cast<std::size_t>(playing_)];
        // A sequence starts at its start time, whatever its phase.
        const double now =
            clip.local_time((clip.start - clip.phase) / clip.frequency + seconds_into_clip);
        apply_clip(static_cast<std::size_t>(playing_), now);
        const String name = clip.name;
        const double before = last_local_;
        for (const auto& [time, text] : clip.text_keys) {
            // Keys passed since the last evaluation, across a loop's wrap.
            const bool passed = now >= before ? (time > before && time <= now)
                                              : (time > before || time <= now);
            if (passed) {
                emit_signal("text_key", name, text);
            }
        }
        last_local_ = now;
        if (clip.cycle == Cycle::clamp && !finished_ &&
            seconds_into_clip * clip.frequency >= clip.stop - clip.start) {
            finished_ = true;
            emit_signal("finished", name);
        }
    }
    flush();
}

void SkydotAnimator::apply_clip(std::size_t index, double local_time) {
    const Clip& clip = asset_->clips[index];
    float v[4] = {0, 0, 0, 0};
    for (const Channel& ch : clip.channels) {
        Target& t = targets_[ch.target];
        const std::size_t slot = ch.target;
        // Bones take transforms only.
        const bool transform = ch.prop >= Prop::translation && ch.prop <= Prop::scale;
        if (t.node == nullptr && (t.skeleton == nullptr || !transform)) {
            continue;
        }
        ch.sample(local_time, v);
        switch (ch.prop) {
        case Prop::translation:
            t.has_translation = true;
            t.translation = Vector3(v[0], v[1], v[2]);
            break;
        case Prop::rotation:
            t.has_rotation = true;
            t.rotation = Quaternion(v[0], v[1], v[2], v[3]).normalized();
            break;
        case Prop::rotation_x:
        case Prop::rotation_y:
        case Prop::rotation_z:
            t.has_euler = true;
            t.euler[static_cast<int>(ch.prop) - static_cast<int>(Prop::rotation_x)] = v[0];
            break;
        case Prop::scale:
            t.has_scale = true;
            t.scale = v[0];
            break;
        case Prop::visible:
            if (t.node->is_visible() != (v[0] > 0.5f)) {
                t.node->set_visible(v[0] > 0.5f);
            }
            break;
        case Prop::birth_rate:
        case Prop::emitter_active:
            if (!t.particles_bound) {
                t.particles_bound = true;
                for (std::int32_t i = 0; i < t.node->get_child_count(); ++i) {
                    if (auto* p = godot::Object::cast_to<SkydotParticles>(t.node->get_child(i))) {
                        t.particles = p;
                    }
                }
            }
            if (t.particles != nullptr) {
                if (ch.prop == Prop::birth_rate) {
                    t.particles->set_birth_rate(static_cast<double>(v[0]));
                } else {
                    t.particles->set_active(v[0] > 0.5f);
                }
            }
            break;
        case Prop::unknown:
            break;
        default:
            bind_material(t, slot);
            if (t.material.is_null()) {
                break;
            }
            switch (ch.prop) {
            case Prop::alpha_test_ref:
                t.alpha_cutoff = v[0] / 255.0f;
                t.dirty |= k_alpha_cutoff;
                break;
            case Prop::emissive_multiple:
                t.emission_strength = v[0];
                t.dirty |= k_emission_strength;
                break;
            case Prop::alpha:
                // Effect shaders keep alpha in the emissive colour.
                if (t.effect) {
                    t.emission.a = v[0];
                    t.dirty |= k_emission;
                } else {
                    t.base_color.a = v[0];
                    t.dirty |= k_base_color;
                }
                break;
            case Prop::u_offset:
                t.uv_offset.x = v[0];
                t.dirty |= k_uv_offset;
                break;
            case Prop::v_offset:
                t.uv_offset.y = v[0];
                t.dirty |= k_uv_offset;
                break;
            case Prop::u_scale:
                t.uv_scale.x = v[0];
                t.dirty |= k_uv_scale;
                break;
            case Prop::v_scale:
                t.uv_scale.y = v[0];
                t.dirty |= k_uv_scale;
                break;
            // The property stores the angles as cosines, its controllers key
            // them in degrees (CloudDistant01: 0.342 and 63-73).
            case Prop::falloff_start_angle:
                t.falloff.x = std::cos(v[0] * static_cast<float>(std::numbers::pi / 180.0));
                t.dirty |= k_falloff;
                break;
            case Prop::falloff_stop_angle:
                t.falloff.y = std::cos(v[0] * static_cast<float>(std::numbers::pi / 180.0));
                t.dirty |= k_falloff;
                break;
            case Prop::falloff_start_opacity:
                t.falloff.z = v[0];
                t.dirty |= k_falloff;
                break;
            case Prop::falloff_stop_opacity:
                t.falloff.w = v[0];
                t.dirty |= k_falloff;
                break;
            case Prop::emissive_color: {
                // Materials hold the NIF's own colour (see SkydotMaterials).
                t.emission = Color(v[0], v[1], v[2], t.emission.a);
                t.dirty |= k_emission;
                break;
            }
            case Prop::glossiness:
                t.glossiness = v[0];
                t.dirty |= k_glossiness;
                break;
            case Prop::specular_strength:
                t.specular_strength = v[0];
                t.dirty |= k_specular_strength;
                break;
            case Prop::specular_color:
                t.specular_color = Vector3(v[0], v[1], v[2]);
                t.dirty |= k_specular_color;
                break;
            case Prop::environment_map_scale:
                t.env_scale = v[0];
                t.dirty |= k_env_scale;
                break;
            default:
                break;
            }
            break;
        }
    }
}

void SkydotAnimator::bind_material(Target& t, std::size_t slot) {
    if (t.material_bound) {
        return;
    }
    t.material_bound = true;
    t.shared = !asset_->own_material[slot];
    godot::MeshInstance3D* instance = mesh_of(t.node);
    if (instance == nullptr || instance->get_mesh().is_null()) {
        return;
    }
    const Ref<godot::Mesh> mesh = instance->get_mesh();
    for (std::int32_t s = 0; s < mesh->get_surface_count(); ++s) {
        Ref<godot::ShaderMaterial> shared = instance->get_surface_override_material(s);
        if (shared.is_null()) {
            shared = mesh->surface_get_material(s);
        }
        if (shared.is_null()) {
            continue;
        }
        // A copy per instance when a sequence drives it, so opening one
        // chest does not open them all.
        Ref<godot::ShaderMaterial> own = t.shared ? shared : Ref<godot::ShaderMaterial>(shared->duplicate());
        instance->set_surface_override_material(s, own);
        if (t.material.is_null()) {
            t.material = own;
        }
    }
    if (t.material.is_null()) {
        return;
    }
    const Ref<godot::ShaderMaterial>& m = t.material;
    t.effect = m->get_shader_parameter("falloff_params").get_type() == Variant::VECTOR4;
    t.uv_offset = param(m, "uv_offset", Vector2(0, 0));
    t.uv_scale = param(m, "uv_scale", Vector2(1, 1));
    t.emission = param_color(m, "emission_color", Color(1, 1, 1, 1));
    t.emission_strength = param_float(m, "emission_strength", 1.0f);
    t.falloff = param(m, "falloff_params", Vector4(1, 1, 0, 0));
    t.base_color = param_color(m, "base_color", Color(1, 1, 1, 1));
    t.glossiness = param_float(m, "glossiness", 30.0f);
    t.specular_strength = param_float(m, "specular_strength", 1.0f);
    t.specular_color = param(m, "specular_color", Vector3(1, 1, 1));
    t.env_scale = param_float(m, "env_scale", 1.0f);
    t.alpha_cutoff = param_float(m, "alpha_cutoff", 0.5f);
}

godot::Transform3D SkydotAnimator::pose_of(const Target& t) {
    Quaternion q = t.has_rotation ? t.rotation : t.rest_rotation;
    if (t.has_euler) {
        // X first, then Y, then Z, as Gamebryo composes them.
        q = Quaternion(Vector3(0, 0, 1), t.euler.z) * Quaternion(Vector3(0, 1, 0), t.euler.y) *
            Quaternion(Vector3(1, 0, 0), t.euler.x);
    }
    const Vector3 scale = t.has_scale ? Vector3(t.scale, t.scale, t.scale) : t.rest_scale;
    const Vector3 origin = t.has_translation ? t.translation : t.rest.origin;
    return {godot::Basis(q).scaled(scale), origin};
}

void SkydotAnimator::flush() {
    for (Target& t : targets_) {
        if (t.skeleton != nullptr) {
            if (t.has_translation || t.has_rotation || t.has_euler || t.has_scale) {
                t.skeleton->set_bone_pose(t.bone, pose_of(t));
                t.has_translation = t.has_rotation = t.has_euler = t.has_scale = false;
            }
            continue;
        }
        if (t.node == nullptr) {
            continue;
        }
        if (t.has_translation || t.has_rotation || t.has_euler || t.has_scale) {
            t.node->set_transform(pose_of(t));
            t.has_translation = t.has_rotation = t.has_euler = t.has_scale = false;
            // The pose has the node's rest rotation; a billboard node must
            // face the camera again. Otherwise the Whiterun brazier's glow
            // card (FXfireWithEmbers01, a scale pulse on its billboard node)
            // showed its authored orientation, oblique and off the fire, and
            // added almost nothing over the basket where the game draws a
            // bright halo (game-refs-2026-10-11, brazier_noglow.txt).
            if (auto* billboard =
                    godot::Object::cast_to<SkydotBillboard>(t.node->get_node_or_null("SkydotBillboard"))) {
                billboard->face_camera();
            }
        }
        if (t.dirty != 0 && t.material.is_valid() && t.shared) {
            const std::uint64_t frame = godot::Engine::get_singleton()->get_process_frames();
            auto [it, added] = shared_writes().try_emplace(t.material->get_instance_id(), frame);
            if (!added && it->second == frame) {
                t.dirty = 0;
            } else {
                it->second = frame;
            }
        }
        if (t.dirty != 0 && t.material.is_valid()) {
            const Ref<godot::ShaderMaterial>& m = t.material;
            if ((t.dirty & k_uv_offset) != 0) {
                m->set_shader_parameter("uv_offset", t.uv_offset);
            }
            if ((t.dirty & k_uv_scale) != 0) {
                m->set_shader_parameter("uv_scale", t.uv_scale);
            }
            if ((t.dirty & k_emission) != 0) {
                m->set_shader_parameter("emission_color", t.effect ? Variant(shader_rgba(t.emission))
                                                                   : Variant(shader_rgb(t.emission)));
            }
            if ((t.dirty & k_emission_strength) != 0) {
                m->set_shader_parameter("emission_strength", t.emission_strength);
            }
            if ((t.dirty & k_falloff) != 0) {
                m->set_shader_parameter("falloff_params", t.falloff);
            }
            if ((t.dirty & k_base_color) != 0) {
                m->set_shader_parameter("base_color", shader_rgba(t.base_color));
            }
            if ((t.dirty & k_glossiness) != 0) {
                m->set_shader_parameter("glossiness", t.glossiness);
            }
            if ((t.dirty & k_specular_strength) != 0) {
                m->set_shader_parameter("specular_strength", t.specular_strength);
            }
            if ((t.dirty & k_specular_color) != 0) {
                m->set_shader_parameter("specular_color", t.specular_color);
            }
            if ((t.dirty & k_env_scale) != 0) {
                m->set_shader_parameter("env_scale", t.env_scale);
            }
            if ((t.dirty & k_alpha_cutoff) != 0) {
                m->set_shader_parameter("alpha_cutoff", t.alpha_cutoff);
            }
            t.dirty = 0;
        }
    }
}

} // namespace skydot
