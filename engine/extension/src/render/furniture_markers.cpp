// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/furniture_markers.hpp"

#include "render/effect_asset.hpp"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace skydot {

using godot::Array;
using godot::Dictionary;
using godot::Variant;

namespace {

float number(const Dictionary& entry, const char* key) {
    const Variant value = entry.get(key, Variant());
    const bool numeric = value.get_type() == Variant::INT || value.get_type() == Variant::FLOAT;
    return numeric ? static_cast<float>(static_cast<double>(value)) : 0.0F;
}

} // namespace

std::vector<FurnitureMarker> furniture_markers(const godot::Node* model) {
    std::vector<FurnitureMarker> out;
    const Variant extra = bethconv_model_extra(model, "furniture_markers");
    if (extra.get_type() != Variant::ARRAY) {
        return out;
    }
    const Array list = extra;
    for (std::int64_t i = 0; i < list.size(); ++i) {
        if (list[i].get_type() != Variant::DICTIONARY) {
            continue;
        }
        const Dictionary entry = list[i];
        FurnitureMarker marker;
        const Variant offset = entry.get("offset", Variant());
        if (offset.get_type() == Variant::ARRAY) {
            const Array xyz = offset;
            for (std::int64_t k = 0; k < 3 && k < xyz.size(); ++k) {
                marker.offset[static_cast<std::size_t>(k)] = static_cast<float>(static_cast<double>(xyz[k]));
            }
        }
        marker.heading = number(entry, "heading");
        marker.animation_type = static_cast<std::uint16_t>(number(entry, "animation_type"));
        marker.entry_points = static_cast<std::uint16_t>(number(entry, "entry_points"));
        out.push_back(marker);
    }
    return out;
}

} // namespace skydot
