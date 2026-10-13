// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sound descriptors (SNDR), sound markers (SOUN) and output models (SOPM) as
// world.fb carries them. Playing them is the game layer's job
// (game/game/sounds.gd).
#include "world/queries.hpp"
#include "data/fb_search.hpp"
#include "data/text.hpp"
#include "build/refs.hpp"

#include "skydot_formats/vpath.hpp"

using godot::Array;
using godot::Dictionary;
using godot::String;

namespace skydot::queries {

std::int64_t find_sound_descriptor(const WorldData& data, const String& editor_id) {
    const auto* all = data.root() != nullptr ? data.root()->sound_descriptors() : nullptr;
    if (all == nullptr) {
        return 0;
    }
    const std::string wanted = formats::ascii_lower(std::string(editor_id.utf8().get_data()));
    for (const auto* d : *all) {
        if (d->editor_id() != nullptr && formats::ascii_lower(d->editor_id()->string_view()) == wanted) {
            return d->id();
        }
    }
    return 0;
}

Dictionary get_sound_descriptor(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* d = lookup(data.root() != nullptr ? data.root()->sound_descriptors() : nullptr,
                           static_cast<std::uint32_t>(id));
    if (d == nullptr) {
        return out;
    }
    out["id"] = static_cast<std::int64_t>(d->id());
    out["editor_id"] = to_godot(d->editor_id());
    Array files;
    if (d->files() != nullptr) {
        for (const auto* f : *d->files()) {
            files.push_back(to_godot(f));
        }
    }
    out["files"] = files;
    out["output_model"] = static_cast<std::int64_t>(d->output_model());
    out["category"] = static_cast<std::int64_t>(d->category());
    out["descriptor_type"] = static_cast<std::int64_t>(d->descriptor_type());
    out["looping"] = static_cast<std::int64_t>(d->looping());
    out["frequency_shift"] = static_cast<std::int64_t>(d->frequency_shift());
    out["frequency_variance"] = static_cast<std::int64_t>(d->frequency_variance());
    out["priority"] = static_cast<std::int64_t>(d->priority());
    out["db_variance"] = static_cast<std::int64_t>(d->db_variance());
    out["static_attenuation"] = static_cast<std::int64_t>(d->static_attenuation());
    return out;
}

std::int64_t find_sound_marker(const WorldData& data, const String& editor_id) {
    const auto* all = data.root() != nullptr ? data.root()->sound_markers() : nullptr;
    if (all == nullptr) {
        return 0;
    }
    const std::string wanted = formats::ascii_lower(std::string(editor_id.utf8().get_data()));
    for (const auto* m : *all) {
        if (m->editor_id() != nullptr && formats::ascii_lower(m->editor_id()->string_view()) == wanted) {
            return m->id();
        }
    }
    return 0;
}

Dictionary get_sound_marker(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* m = lookup(data.root() != nullptr ? data.root()->sound_markers() : nullptr,
                           static_cast<std::uint32_t>(id));
    if (m == nullptr) {
        return out;
    }
    out["id"] = static_cast<std::int64_t>(m->id());
    out["editor_id"] = to_godot(m->editor_id());
    out["descriptor"] = static_cast<std::int64_t>(m->descriptor());
    return out;
}

Dictionary get_sound_output_model(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* m = lookup(data.root() != nullptr ? data.root()->sound_output_models() : nullptr,
                           static_cast<std::uint32_t>(id));
    if (m == nullptr) {
        return out;
    }
    out["id"] = static_cast<std::int64_t>(m->id());
    out["editor_id"] = to_godot(m->editor_id());
    out["flags"] = static_cast<std::int64_t>(m->flags());
    out["type"] = static_cast<std::int64_t>(m->type());
    out["min_distance"] = static_cast<double>(m->min_distance());
    out["max_distance"] = static_cast<double>(m->max_distance());
    godot::PackedByteArray curve;
    if (m->curve() != nullptr) {
        for (const auto c : *m->curve()) {
            curve.push_back(c);
        }
    }
    out["curve"] = curve;
    return out;
}

} // namespace skydot::queries
