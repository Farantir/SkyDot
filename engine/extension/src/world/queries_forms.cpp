// SPDX-License-Identifier: GPL-3.0-or-later
//
// Keywords (KYWD, and KWDA on bases), form lists (FLST) and messages (MESG)
// as world.fb carries them.
#include "world/queries.hpp"
#include "data/fb_search.hpp"
#include "data/text.hpp"

#include "skydot_formats/flags.hpp"
#include "skydot_formats/vpath.hpp"
#include "build/refs.hpp"
#include "world_generated.h"

using godot::Array;
using godot::Dictionary;
using godot::String;

namespace wfb = bethconv::pack::wfb;

namespace skydot::queries {

std::int64_t find_keyword(const WorldData& data, const String& editor_id) {
    return data.keyword_by_editor_id(std::string_view(editor_id.utf8().get_data()));
}

godot::PackedInt64Array get_base_keywords(const WorldData& data, std::int64_t base) {
    godot::PackedInt64Array out;
    const auto* b = lookup(data.root() != nullptr ? data.root()->bases() : nullptr, static_cast<std::uint32_t>(base));
    if (b != nullptr && b->keywords() != nullptr) {
        for (const auto k : *b->keywords()) {
            out.push_back(k);
        }
    }
    return out;
}

Dictionary get_form_list(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* l = lookup(data.root() != nullptr ? data.root()->form_lists() : nullptr, static_cast<std::uint32_t>(id));
    if (l == nullptr) {
        return out;
    }
    out["editor_id"] = to_godot(l->editor_id());
    godot::PackedInt64Array forms;
    if (l->forms() != nullptr) {
        for (const auto f : *l->forms()) {
            forms.push_back(f);
        }
    }
    out["forms"] = forms;
    return out;
}

Dictionary get_message(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* m = lookup(data.root() != nullptr ? data.root()->messages() : nullptr, static_cast<std::uint32_t>(id));
    if (m == nullptr) {
        return out;
    }
    out["editor_id"] = to_godot(m->editor_id());
    out["title"] = to_godot(m->title());
    out["text"] = to_godot(m->text());
    out["box"] = formats::has_flag(m->flags(), wfb::MessageFlags::message_box);
    Array buttons;
    if (m->buttons() != nullptr) {
        for (const auto* b : *m->buttons()) {
            buttons.push_back(to_godot(b));
        }
    }
    out["buttons"] = buttons;
    return out;
}

/// health, flags, stages: health_percent, index, damage_stage, flags,
/// self_damage, explosion, debris, debris_count, model (a path, or empty).
Dictionary get_destruction(const WorldData& data, std::int64_t base) {
    Dictionary out;
    const auto* b = lookup(data.root() != nullptr ? data.root()->bases() : nullptr, static_cast<std::uint32_t>(base));
    if (b == nullptr || b->destruction() == nullptr) {
        return out;
    }
    const auto* d = b->destruction();
    out["health"] = static_cast<std::int64_t>(d->health());
    out["flags"] = static_cast<std::int64_t>(d->flags());
    Array stages;
    if (d->stages() != nullptr) {
        for (const auto* st : *d->stages()) {
            Dictionary stage;
            stage["health_percent"] = static_cast<std::int64_t>(st->health_percent());
            stage["index"] = static_cast<std::int64_t>(st->index());
            stage["damage_stage"] = static_cast<std::int64_t>(st->damage_stage());
            stage["flags"] = static_cast<std::int64_t>(st->flags());
            stage["self_damage"] = static_cast<std::int64_t>(st->self_damage_per_second());
            stage["explosion"] = static_cast<std::int64_t>(st->explosion());
            stage["debris"] = static_cast<std::int64_t>(st->debris());
            stage["debris_count"] = static_cast<std::int64_t>(st->debris_count());
            stage["model"] = to_godot(st->model());
            stages.push_back(stage);
        }
    }
    out["stages"] = stages;
    return out;
}

Dictionary get_explosion(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* e = lookup(data.root() != nullptr ? data.root()->explosions() : nullptr, static_cast<std::uint32_t>(id));
    if (e == nullptr) {
        return out;
    }
    out["editor_id"] = to_godot(e->editor_id());
    out["model"] = to_godot(e->model());
    out["light"] = static_cast<std::int64_t>(e->light());
    out["sound"] = static_cast<std::int64_t>(e->sound());
    out["sound_far"] = static_cast<std::int64_t>(e->sound_far());
    out["impact_data"] = static_cast<std::int64_t>(e->impact_data());
    out["placed_object"] = static_cast<std::int64_t>(e->placed_object());
    out["projectile"] = static_cast<std::int64_t>(e->projectile());
    out["force"] = e->force();
    out["damage"] = e->damage();
    out["radius"] = e->radius();
    out["flags"] = static_cast<std::int64_t>(e->flags());
    return out;
}

} // namespace skydot::queries
