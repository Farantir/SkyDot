// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the menus and the HUD read from world.fb (format 43): the map markers a
// compass shows, the default objects (DOBJ) and image space modifiers (IMAD)
// such as the one the inventory menu puts over the world.
#include "world/queries.hpp"
#include "data/fb_search.hpp"
#include "data/text.hpp"

#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

using godot::Array;
using godot::Dictionary;
using godot::String;

namespace skydot::queries {

Array get_map_markers(const WorldData& data, std::int64_t world) {
    Array out;
    const auto* cells = data.root() != nullptr ? data.root()->cells() : nullptr;
    if (cells == nullptr) {
        return out;
    }
    for (const auto* cell : *cells) {
        if (cell->map_markers() == nullptr || static_cast<std::int64_t>(cell->world()) != world) {
            continue;
        }
        for (const auto* marker : *cell->map_markers()) {
            const auto* ref = lookup(cell->refs(), marker->ref());
            if (ref == nullptr) {
                continue;
            }
            Dictionary m;
            m["ref"] = static_cast<std::int64_t>(marker->ref());
            m["type"] = static_cast<std::int64_t>(marker->type());
            m["flags"] = static_cast<std::int64_t>(marker->flags());
            m["name"] = to_godot(marker->name());
            m["position"] = godot::Vector3(ref->position().x(), ref->position().y(), ref->position().z());
            out.push_back(m);
        }
    }
    return out;
}

std::int64_t get_default_object(const WorldData& data, const String& key) {
    const auto* all = data.root() != nullptr ? data.root()->default_objects() : nullptr;
    const auto bytes = key.ascii();
    if (all == nullptr || bytes.length() != 4) {
        return 0;
    }
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i) {
        value = (value << 8) | static_cast<std::uint8_t>(bytes.get_data()[i]);
    }
    const auto* found = lookup(all, value);
    return found != nullptr ? static_cast<std::int64_t>(found->form()) : 0;
}

Dictionary get_image_space_modifier(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* m = lookup(data.root() != nullptr ? data.root()->image_space_modifiers() : nullptr,
                           static_cast<std::uint32_t>(id));
    if (m == nullptr) {
        return out;
    }
    const auto floats = [](const flatbuffers::Vector<float>* v) {
        godot::PackedFloat32Array a;
        if (v != nullptr) {
            for (const float f : *v) {
                a.push_back(static_cast<double>(f));
            }
        }
        return a;
    };
    out["id"] = static_cast<std::int64_t>(m->id());
    out["editor_id"] = to_godot(m->editor_id());
    out["duration"] = m->duration();
    out["dof_strength"] = m->dof_strength();
    out["dof_distance"] = m->dof_distance();
    out["dof_range"] = m->dof_range();
    out["blur_radius"] = m->blur_radius();
    out["motion_blur"] = m->motion_blur();
    out["radial_blur"] = m->radial_blur();
    out["tint"] = floats(m->tint());
    out["fade_color"] = floats(m->fade_color());
    out["multiply"] = floats(m->multiply());
    out["add"] = floats(m->add());
    return out;
}

} // namespace skydot::queries
