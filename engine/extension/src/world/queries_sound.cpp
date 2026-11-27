// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sound descriptors (SNDR), sound markers (SOUN) and output models (SOPM) as
// world.fb carries them. Playing them is the game layer's job
// (game/game/sounds.gd).
#include "world/queries.hpp"
#include "data/fb_search.hpp"
#include "data/text.hpp"
#include "build/refs.hpp"

#include "skydot_formats/flags.hpp"
#include "skydot_formats/vpath.hpp"
#include "world_generated.h"

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
    out["alternate_for"] = static_cast<std::int64_t>(d->alternate_for());
    out["has_conditions"] = d->conditions() != nullptr && d->conditions()->size() > 0;
    out["rumble"] = static_cast<std::int64_t>(d->rumble());
    return out;
}

godot::PackedInt64Array get_sound_alternates(const WorldData& data, std::int64_t id) {
    godot::PackedInt64Array out;
    const auto* all = data.root() != nullptr ? data.root()->sound_descriptors() : nullptr;
    if (all == nullptr || id == 0) {
        return out;
    }
    for (const auto* d : *all) {
        if (d->alternate_for() == static_cast<std::uint32_t>(id)) {
            out.push_back(d->id());
        }
    }
    return out;
}

Dictionary get_sound_category(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* c = lookup(data.root() != nullptr ? data.root()->sound_categories() : nullptr,
                           static_cast<std::uint32_t>(id));
    if (c == nullptr) {
        return out;
    }
    out["id"] = static_cast<std::int64_t>(c->id());
    out["editor_id"] = to_godot(c->editor_id());
    out["name"] = to_godot(c->name());
    out["flags"] = static_cast<std::int64_t>(c->flags());
    out["parent"] = static_cast<std::int64_t>(c->parent());
    out["static_volume"] = static_cast<double>(c->static_volume());
    out["default_menu_volume"] = static_cast<double>(c->default_menu_volume());
    return out;
}

Dictionary get_acoustic_space(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* a = lookup(data.root() != nullptr ? data.root()->acoustic_spaces() : nullptr,
                           static_cast<std::uint32_t>(id));
    if (a == nullptr) {
        return out;
    }
    out["id"] = static_cast<std::int64_t>(a->id());
    out["editor_id"] = to_godot(a->editor_id());
    out["ambient"] = static_cast<std::int64_t>(a->ambient());
    out["region"] = static_cast<std::int64_t>(a->region());
    out["reverb"] = static_cast<std::int64_t>(a->reverb());
    return out;
}

Dictionary get_reverb(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* r = lookup(data.root() != nullptr ? data.root()->reverbs() : nullptr,
                           static_cast<std::uint32_t>(id));
    if (r == nullptr) {
        return out;
    }
    out["id"] = static_cast<std::int64_t>(r->id());
    out["editor_id"] = to_godot(r->editor_id());
    out["decay_time_ms"] = static_cast<std::int64_t>(r->decay_time_ms());
    out["hf_reference_hz"] = static_cast<std::int64_t>(r->hf_reference_hz());
    out["room_filter"] = static_cast<std::int64_t>(r->room_filter());
    out["room_hf_filter"] = static_cast<std::int64_t>(r->room_hf_filter());
    out["reflections"] = static_cast<std::int64_t>(r->reflections());
    out["reverb_amplitude"] = static_cast<std::int64_t>(r->reverb_amplitude());
    out["decay_hf_ratio"] = static_cast<std::int64_t>(r->decay_hf_ratio());
    out["reflect_delay"] = static_cast<std::int64_t>(r->reflect_delay());
    out["reverb_delay_ms"] = static_cast<std::int64_t>(r->reverb_delay_ms());
    out["diffusion_percent"] = static_cast<std::int64_t>(r->diffusion_percent());
    out["density_percent"] = static_cast<std::int64_t>(r->density_percent());
    return out;
}

namespace {

bool inside_polygon(const flatbuffers::Vector<float>* points, double x, double y) {
    if (points == nullptr || points->size() < 6) {
        return false;
    }
    bool in = false;
    const flatbuffers::uoffset_t n = points->size() / 2;
    for (flatbuffers::uoffset_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = static_cast<double>(points->Get(2 * i));
        const double yi = static_cast<double>(points->Get(2 * i + 1));
        const double xj = static_cast<double>(points->Get(2 * j));
        const double yj = static_cast<double>(points->Get(2 * j + 1));
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
            in = !in;
        }
    }
    return in;
}

Dictionary region_sound_dict(const bethconv::pack::wfb::Region& r) {
    Dictionary out;
    out["id"] = static_cast<std::int64_t>(r.id());
    out["editor_id"] = to_godot(r.editor_id());
    out["priority"] = static_cast<std::int64_t>(r.sound_priority());
    out["override"] = r.sound_override();
    Array sounds;
    if (r.sounds() != nullptr) {
        for (const auto* s : *r.sounds()) {
            Dictionary e;
            e["sound"] = static_cast<std::int64_t>(s->sound());
            e["flags"] = static_cast<std::int64_t>(s->flags());
            e["chance"] = static_cast<double>(s->chance());
            sounds.push_back(e);
        }
    }
    out["sounds"] = sounds;
    out["music"] = static_cast<std::int64_t>(r.music());
    return out;
}

/// The id of the record named `editor_id` (any case) in `all`, 0 if none.
template <typename List>
std::int64_t find_by_editor_id(const List* all, const String& editor_id) {
    if (all == nullptr) {
        return 0;
    }
    const std::string wanted = formats::ascii_lower(std::string(editor_id.utf8().get_data()));
    for (const auto* r : *all) {
        if (r->editor_id() != nullptr && formats::ascii_lower(r->editor_id()->string_view()) == wanted) {
            return r->id();
        }
    }
    return 0;
}

} // namespace

Array get_region_sounds(const WorldData& data, std::int64_t world, double x, double y) {
    Array out;
    const auto* regions = data.root() != nullptr ? data.root()->regions() : nullptr;
    if (regions == nullptr) {
        return out;
    }
    // A child worldspace uses its parent's regions too, as the weather does.
    const auto* ws = data.world_ptr(static_cast<std::uint32_t>(world));
    const std::uint32_t parent = ws != nullptr ? ws->parent() : 0;
    for (const auto* r : *regions) {
        if (r->sounds() == nullptr || r->sounds()->size() == 0 || r->areas() == nullptr ||
            (r->world() != static_cast<std::uint32_t>(world) && (parent == 0 || r->world() != parent))) {
            continue;
        }
        for (const auto* area : *r->areas()) {
            if (inside_polygon(area->points(), x, y)) {
                out.push_back(region_sound_dict(*r));
                break;
            }
        }
    }
    return out;
}

Array get_region_music(const WorldData& data, std::int64_t world, double x, double y) {
    Array out;
    const auto* regions = data.root() != nullptr ? data.root()->regions() : nullptr;
    if (regions == nullptr) {
        return out;
    }
    // A child worldspace uses its parent's regions too, as the sounds do.
    const auto* ws = data.world_ptr(static_cast<std::uint32_t>(world));
    const std::uint32_t parent = ws != nullptr ? ws->parent() : 0;
    for (const auto* r : *regions) {
        if (r->music() == 0 || r->areas() == nullptr ||
            (r->world() != static_cast<std::uint32_t>(world) && (parent == 0 || r->world() != parent))) {
            continue;
        }
        for (const auto* area : *r->areas()) {
            if (inside_polygon(area->points(), x, y)) {
                out.push_back(region_sound_dict(*r));
                break;
            }
        }
    }
    return out;
}

godot::PackedInt64Array get_music_types(const WorldData& data) {
    godot::PackedInt64Array out;
    if (const auto* all = data.root() != nullptr ? data.root()->music_types() : nullptr) {
        for (const auto* m : *all) {
            out.push_back(static_cast<std::int64_t>(m->id()));
        }
    }
    return out;
}

Dictionary get_music_type(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* m = lookup(data.root() != nullptr ? data.root()->music_types() : nullptr,
                           static_cast<std::uint32_t>(id));
    if (m == nullptr) {
        return out;
    }
    using F = bethconv::pack::wfb::MusicTypeFlags;
    out["id"] = static_cast<std::int64_t>(m->id());
    out["editor_id"] = to_godot(m->editor_id());
    out["flags"] = static_cast<std::int64_t>(m->flags());
    out["plays_one_selection"] = formats::has_flag(m->flags(), F::plays_one_selection);
    out["abrupt_transition"] = formats::has_flag(m->flags(), F::abrupt_transition);
    out["cycle_tracks"] = formats::has_flag(m->flags(), F::cycle_tracks);
    out["maintain_track_order"] = formats::has_flag(m->flags(), F::maintain_track_order);
    out["ducks_current_track"] = formats::has_flag(m->flags(), F::ducks_current_track);
    out["doesnt_queue"] = formats::has_flag(m->flags(), F::doesnt_queue);
    out["priority"] = static_cast<std::int64_t>(m->priority());
    out["ducking_db"] = static_cast<double>(m->ducking()) / 100.0;
    out["fade_duration"] = static_cast<double>(m->fade_duration());
    out["tracks"] = ids_array(m->tracks());
    return out;
}

Dictionary get_music_track(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* t = lookup(data.root() != nullptr ? data.root()->music_tracks() : nullptr,
                           static_cast<std::uint32_t>(id));
    if (t == nullptr) {
        return out;
    }
    static constexpr const char* k_types[] = {"single", "palette", "silent", "unknown"};
    const auto type = static_cast<std::size_t>(t->track_type());
    out["id"] = static_cast<std::int64_t>(t->id());
    out["editor_id"] = to_godot(t->editor_id());
    out["type"] = String(k_types[type < 4 ? type : 3]);
    out["duration"] = static_cast<double>(t->duration());
    out["fade_out"] = static_cast<double>(t->fade_out());
    out["file"] = to_godot(t->file());
    out["finale"] = to_godot(t->finale());
    godot::PackedFloat64Array cues;
    if (t->cue_points() != nullptr) {
        for (const float c : *t->cue_points()) {
            cues.push_back(static_cast<double>(c));
        }
    }
    out["cue_points"] = cues;
    out["loop_begin"] = static_cast<double>(t->loop_begin());
    out["loop_end"] = static_cast<double>(t->loop_end());
    out["loop_count"] = static_cast<std::int64_t>(t->loop_count());
    out["has_conditions"] = t->conditions() != nullptr && t->conditions()->size() > 0;
    out["tracks"] = ids_array(t->tracks());
    return out;
}

std::int64_t find_music_type(const WorldData& data, const String& editor_id) {
    return find_by_editor_id(data.root() != nullptr ? data.root()->music_types() : nullptr, editor_id);
}

std::int64_t find_music_track(const WorldData& data, const String& editor_id) {
    return find_by_editor_id(data.root() != nullptr ? data.root()->music_tracks() : nullptr, editor_id);
}

std::int64_t get_cell_music(const WorldData& data, std::int64_t cell) {
    const auto* c = data.cell_ptr(cell);
    return c != nullptr ? static_cast<std::int64_t>(c->music()) : 0;
}

std::int64_t get_world_music(const WorldData& data, std::int64_t world) {
    const auto* w = data.world_ptr(static_cast<std::uint32_t>(world));
    return w != nullptr ? static_cast<std::int64_t>(w->music()) : 0;
}

std::int64_t get_location_music(const WorldData& data, std::int64_t location) {
    const auto* l = lookup(data.root() != nullptr ? data.root()->locations() : nullptr,
                           static_cast<std::uint32_t>(location));
    return l != nullptr ? static_cast<std::int64_t>(l->music()) : 0;
}

Dictionary get_region_sound_data(const WorldData& data, std::int64_t region) {
    const auto* r = lookup(data.root() != nullptr ? data.root()->regions() : nullptr,
                           static_cast<std::uint32_t>(region));
    return r != nullptr ? region_sound_dict(*r) : Dictionary();
}

Array get_weather_sounds(const WorldData& data, std::int64_t weather) {
    Array out;
    const auto* w = lookup(data.root() != nullptr ? data.root()->weathers() : nullptr,
                           static_cast<std::uint32_t>(weather));
    if (w == nullptr || w->sounds() == nullptr) {
        return out;
    }
    for (const auto* s : *w->sounds()) {
        Dictionary e;
        e["sound"] = static_cast<std::int64_t>(s->sound());
        e["type"] = static_cast<std::int64_t>(s->type());
        out.push_back(e);
    }
    return out;
}

std::int64_t get_weather_classification(const WorldData& data, std::int64_t weather) {
    const auto* w = lookup(data.root() != nullptr ? data.root()->weathers() : nullptr,
                           static_cast<std::uint32_t>(weather));
    return w != nullptr ? static_cast<std::int64_t>(w->classification()) : 0;
}

Dictionary get_footstep_set(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* f = lookup(data.root() != nullptr ? data.root()->footstep_sets() : nullptr,
                           static_cast<std::uint32_t>(id));
    if (f == nullptr) {
        return out;
    }
    out["id"] = static_cast<std::int64_t>(f->id());
    out["editor_id"] = to_godot(f->editor_id());
    out["walk"] = ids_array(f->walk());
    out["run"] = ids_array(f->run());
    out["sprint"] = ids_array(f->sprint());
    out["sneak"] = ids_array(f->sneak());
    out["swim"] = ids_array(f->swim());
    return out;
}

Dictionary get_footstep(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* f = lookup(data.root() != nullptr ? data.root()->footsteps() : nullptr,
                           static_cast<std::uint32_t>(id));
    if (f == nullptr) {
        return out;
    }
    out["id"] = static_cast<std::int64_t>(f->id());
    out["editor_id"] = to_godot(f->editor_id());
    out["impact_data_set"] = static_cast<std::int64_t>(f->impact_data_set());
    out["tag"] = to_godot(f->tag());
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
    out["reverb_send"] = static_cast<std::int64_t>(m->reverb_send());
    godot::PackedByteArray speakers;
    if (m->speaker_output() != nullptr) {
        for (const auto c : *m->speaker_output()) {
            speakers.push_back(c);
        }
    }
    out["speaker_output"] = speakers;
    return out;
}

} // namespace skydot::queries
