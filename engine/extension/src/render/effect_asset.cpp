// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/effect_asset.hpp"

#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <string>

using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Variant;

namespace skydot {

namespace {

/// Named clips the game starts without being asked, in order of preference.
constexpr const char* k_autoplay_names[] = {"AutoPlay", "AutoLoop", "Idle", "mIdle",
                                            "SpecialIdle", "mLoop"};

struct PropName {
    const char* name;
    Prop prop;
};

// Effect and lighting shader variables share one set; the animator knows which
// uniform each maps to on either shader.
constexpr PropName k_props[] = {
    {"translation", Prop::translation},
    {"rotation", Prop::rotation},
    {"rotation_x", Prop::rotation_x},
    {"rotation_y", Prop::rotation_y},
    {"rotation_z", Prop::rotation_z},
    {"scale", Prop::scale},
    {"visible", Prop::visible},
    {"alpha_test_ref", Prop::alpha_test_ref},
    {"effect.emissive_multiple", Prop::emissive_multiple},
    {"effect.alpha", Prop::alpha},
    {"effect.u_offset", Prop::u_offset},
    {"effect.v_offset", Prop::v_offset},
    {"effect.u_scale", Prop::u_scale},
    {"effect.v_scale", Prop::v_scale},
    {"effect.falloff_start_angle", Prop::falloff_start_angle},
    {"effect.falloff_stop_angle", Prop::falloff_stop_angle},
    {"effect.falloff_start_opacity", Prop::falloff_start_opacity},
    {"effect.falloff_stop_opacity", Prop::falloff_stop_opacity},
    {"effect.emissive_color", Prop::emissive_color},
    {"lighting.emissive_multiple", Prop::emissive_multiple},
    {"lighting.alpha", Prop::alpha},
    {"lighting.u_offset", Prop::u_offset},
    {"lighting.v_offset", Prop::v_offset},
    {"lighting.u_scale", Prop::u_scale},
    {"lighting.v_scale", Prop::v_scale},
    {"lighting.emissive_color", Prop::emissive_color},
    {"lighting.glossiness", Prop::glossiness},
    {"lighting.specular_strength", Prop::specular_strength},
    {"lighting.specular_color", Prop::specular_color},
    {"lighting.environment_map_scale", Prop::environment_map_scale},
    {"particles.birth_rate", Prop::birth_rate},
    {"particles.active", Prop::emitter_active},
};

Prop prop_of(const String& name) {
    for (const PropName& p : k_props) {
        if (name == p.name) {
            return p.prop;
        }
    }
    return Prop::unknown;
}

std::vector<float> floats(const Variant& v) {
    std::vector<float> out;
    if (v.get_type() == Variant::PACKED_FLOAT32_ARRAY || v.get_type() == Variant::PACKED_FLOAT64_ARRAY) {
        const godot::PackedFloat64Array a = v;
        out.reserve(static_cast<std::size_t>(a.size()));
        for (std::int64_t i = 0; i < a.size(); ++i) {
            out.push_back(static_cast<float>(a[i]));
        }
        return out;
    }
    if (v.get_type() != Variant::ARRAY) {
        return out;
    }
    const Array a = v;
    out.reserve(static_cast<std::size_t>(a.size()));
    for (std::int64_t i = 0; i < a.size(); ++i) {
        out.push_back(static_cast<float>(static_cast<double>(a[i])));
    }
    return out;
}

/// Some files list keys out of time order; sampling bisects, so put them in
/// order. False if a time is not a number.
bool sort_keys(Channel& ch) {
    for (const float t : ch.times) {
        if (!std::isfinite(t)) {
            return false;
        }
    }
    if (std::is_sorted(ch.times.begin(), ch.times.end())) {
        return true;
    }
    std::vector<std::size_t> order(ch.times.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return ch.times[a] < ch.times[b]; });
    const std::size_t c = static_cast<std::size_t>(ch.components);
    auto reorder = [&](std::vector<float>& v, std::size_t width) {
        if (v.size() != order.size() * width) {
            return;
        }
        std::vector<float> out(v.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            std::copy_n(v.begin() + static_cast<std::ptrdiff_t>(order[i] * width), width,
                        out.begin() + static_cast<std::ptrdiff_t>(i * width));
        }
        v = std::move(out);
    };
    reorder(ch.times, 1);
    reorder(ch.values, c);
    reorder(ch.in, c);
    reorder(ch.out, c);
    return true;
}

double number(const Dictionary& d, const char* key, double fallback) {
    const Variant v = d.get(key, fallback);
    return v.get_type() == Variant::FLOAT || v.get_type() == Variant::INT ? static_cast<double>(v)
                                                                            : fallback;
}

void scan(godot::Node* node, std::unordered_map<std::int64_t, godot::Node*>& out,
          bool& hidden) {
    const Dictionary extras = bethconv_extras(node);
    if (!extras.is_empty()) {
        const Variant id = extras.get("id", Variant());
        // The importer copies one joint's extras onto the Skeleton3D itself
        // (the cairn banner's carries its Top02 bone's id); that node is not
        // the animated one, the bone is (see bones_by_id in the animator).
        if ((id.get_type() == Variant::INT || id.get_type() == Variant::FLOAT) &&
            godot::Object::cast_to<godot::Skeleton3D>(node) == nullptr) {
            out.try_emplace(static_cast<std::int64_t>(id), node);
        }
        if (static_cast<bool>(extras.get("hidden", false))) {
            hidden = true;
        }
    }
    for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
        scan(node->get_child(i), out, hidden);
    }
}

/// The block holding `animations`/`particles`: the axis-conversion node right
/// under the scene root, or the root itself when the model kept NIF axes.
Dictionary root_block(godot::Node* root) {
    Dictionary block = bethconv_extras(root);
    if (block.has("source")) {
        return block;
    }
    for (std::int32_t i = 0; i < root->get_child_count(); ++i) {
        block = bethconv_extras(root->get_child(i));
        if (block.has("source")) {
            return block;
        }
    }
    return {};
}

std::shared_ptr<EffectAsset> parse_extras(godot::Node* root) {
    auto asset = std::make_shared<EffectAsset>();
    const Dictionary block = root_block(root);
    std::unordered_map<std::int64_t, std::size_t> slot_by_id;
    auto slot = [&](std::int64_t id) {
        auto [it, added] = slot_by_id.try_emplace(id, asset->ids.size());
        if (added) {
            asset->ids.push_back(id);
        }
        return it->second;
    };

    const Variant clips = block.get("animations", Variant());
    if (clips.get_type() == Variant::ARRAY) {
        const Array list = clips;
        for (std::int64_t i = 0; i < list.size(); ++i) {
            if (list[i].get_type() != Variant::DICTIONARY) {
                continue;
            }
            const Dictionary c = list[i];
            Clip clip;
            clip.name = c.get("name", String());
            clip.autoplay = static_cast<bool>(c.get("autoplay", false));
            const String cycle = c.get("cycle", String("loop"));
            clip.cycle = cycle == "clamp" ? Cycle::clamp : cycle == "reverse" ? Cycle::reverse : Cycle::loop;
            clip.frequency = number(c, "frequency", 1.0);
            if (clip.frequency <= 0.0) {
                clip.frequency = 1.0;
            }
            clip.phase = number(c, "phase", 0.0);
            clip.start = number(c, "start", 0.0);
            clip.stop = number(c, "stop", 0.0);
            const Variant keys = c.get("text_keys", Variant());
            if (keys.get_type() == Variant::ARRAY) {
                const Array k = keys;
                for (std::int64_t j = 0; j < k.size(); ++j) {
                    if (k[j].get_type() == Variant::ARRAY && Array(k[j]).size() >= 2) {
                        const Array pair = k[j];
                        clip.text_keys.emplace_back(static_cast<double>(pair[0]), String(pair[1]));
                    }
                }
            }
            const Variant channels = c.get("channels", Variant());
            if (channels.get_type() == Variant::ARRAY) {
                const Array chs = channels;
                for (std::int64_t j = 0; j < chs.size(); ++j) {
                    if (chs[j].get_type() != Variant::DICTIONARY) {
                        continue;
                    }
                    const Dictionary d = chs[j];
                    Channel ch;
                    ch.prop = prop_of(d.get("property", String()));
                    if (ch.prop == Prop::unknown) {
                        continue;
                    }
                    ch.target = slot(static_cast<std::int64_t>(number(d, "node", -1)));
                    const String interp = d.get("interp", String("linear"));
                    ch.interp = interp == "step" ? Interp::step : interp == "cubic" ? Interp::cubic : Interp::linear;
                    ch.components = std::clamp(static_cast<int>(number(d, "components", 1)), 1, 4);
                    ch.times = floats(d.get("times", Variant()));
                    ch.values = floats(d.get("values", Variant()));
                    ch.in = floats(d.get("in", Variant()));
                    ch.out = floats(d.get("out", Variant()));
                    const std::size_t n = ch.times.size() * static_cast<std::size_t>(ch.components);
                    if (ch.times.empty() || ch.values.size() != n) {
                        continue;
                    }
                    if (ch.interp == Interp::cubic && (ch.in.size() != n || ch.out.size() != n)) {
                        ch.interp = Interp::linear;
                    }
                    if (!sort_keys(ch)) {
                        continue;
                    }
                    clip.channels.push_back(std::move(ch));
                }
            }
            if (!clip.channels.empty()) {
                asset->clips.push_back(std::move(clip));
            }
        }
    }
    asset->own_material.assign(asset->ids.size(), false);
    for (const Clip& clip : asset->clips) {
        if (clip.name.is_empty()) {
            continue;
        }
        for (const Channel& ch : clip.channels) {
            if (ch.prop >= Prop::alpha_test_ref && ch.prop <= Prop::environment_map_scale) {
                asset->own_material[ch.target] = true;
            }
        }
    }
    for (const char* name : k_autoplay_names) {
        for (std::size_t i = 0; i < asset->clips.size() && asset->autoplay_sequence < 0; ++i) {
            if (asset->clips[i].name == name) {
                asset->autoplay_sequence = static_cast<int>(i);
            }
        }
    }

    const Variant particles = block.get("particles", Variant());
    if (particles.get_type() == Variant::ARRAY) {
        asset->particles = particles;
    }

    std::unordered_map<std::int64_t, godot::Node*> unused;
    scan(root, unused, asset->has_hidden);
    return asset;
}

} // namespace

Dictionary bethconv_extras(const godot::Node* node) {
    if (node == nullptr || !node->has_meta("extras")) {
        return {};
    }
    const Variant extras = node->get_meta("extras");
    if (extras.get_type() != Variant::DICTIONARY) {
        return {};
    }
    const Variant block = Dictionary(extras).get("bethconv", Variant());
    return block.get_type() == Variant::DICTIONARY ? Dictionary(block) : Dictionary();
}

std::unordered_map<std::int64_t, godot::Node*> nodes_by_id(godot::Node* root) {
    std::unordered_map<std::int64_t, godot::Node*> out;
    bool hidden = false;
    if (root != nullptr) {
        scan(root, out, hidden);
    }
    return out;
}

std::shared_ptr<const EffectAsset> EffectAsset::parse(godot::Node* root) {
    return parse_extras(root);
}

double Clip::local_time(double clock) const {
    const double time = frequency * clock + phase;
    const double span = stop - start;
    if (span <= 0.0) {
        return start;
    }
    if (time >= start && time <= stop) {
        return time;
    }
    switch (cycle) {
    case Cycle::clamp:
        return std::clamp(time, start, stop);
    case Cycle::reverse: {
        const double x = std::fmod(std::fabs(time - start), 2.0 * span);
        return start + (x <= span ? x : 2.0 * span - x);
    }
    case Cycle::loop:
        break;
    }
    const double x = (time - start) / span;
    return start + (x - std::floor(x)) * span;
}

void Channel::sample(double time, float* out_value) const {
    // Compare in the keys' own precision, or a time just below the last key
    // in double can round onto it in float and bisect past the end.
    const float t = static_cast<float>(time);
    const std::size_t c = static_cast<std::size_t>(components);
    const std::size_t n = times.size();
    if (n == 1 || t <= times.front()) {
        std::copy_n(values.begin(), c, out_value);
        return;
    }
    if (t >= times.back()) {
        std::copy_n(values.end() - static_cast<std::ptrdiff_t>(c), c, out_value);
        return;
    }
    const auto upper = std::upper_bound(times.begin(), times.end(), t);
    const std::size_t i = static_cast<std::size_t>(upper - times.begin()) - 1;
    const double t0 = static_cast<double>(times[i]);
    const double t1 = static_cast<double>(times[i + 1]);
    const double s = t1 > t0 ? (static_cast<double>(t) - t0) / (t1 - t0) : 0.0;
    const float* a = values.data() + i * c;
    const float* b = values.data() + (i + 1) * c;
    if (interp == Interp::step) {
        std::copy_n(a, c, out_value);
        return;
    }
    if (prop == Prop::rotation && c == 4) {
        const godot::Quaternion qa(a[0], a[1], a[2], a[3]);
        godot::Quaternion qb(b[0], b[1], b[2], b[3]);
        const godot::Quaternion q = qa.normalized().slerp(qb.normalized(), static_cast<float>(s));
        out_value[0] = q.x;
        out_value[1] = q.y;
        out_value[2] = q.z;
        out_value[3] = q.w;
        return;
    }
    if (interp == Interp::linear) {
        for (std::size_t j = 0; j < c; ++j) {
            out_value[j] = static_cast<float>(static_cast<double>(a[j]) + static_cast<double>(b[j] - a[j]) * s);
        }
        return;
    }
    // Hermite with Gamebryo's per-segment tangents.
    const double s2 = s * s;
    const double s3 = s2 * s;
    const double h00 = 2 * s3 - 3 * s2 + 1;
    const double h10 = s3 - 2 * s2 + s;
    const double h01 = -2 * s3 + 3 * s2;
    const double h11 = s3 - s2;
    const float* out_a = out.data() + i * c;
    const float* in_b = in.data() + (i + 1) * c;
    for (std::size_t j = 0; j < c; ++j) {
        out_value[j] = static_cast<float>(
            h00 * static_cast<double>(a[j]) + h10 * static_cast<double>(out_a[j]) +
            h01 * static_cast<double>(b[j]) + h11 * static_cast<double>(in_b[j]));
    }
}

} // namespace skydot
