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

double get_gmst_float(const WorldData& data, const String& name, double fallback) {
    return static_cast<double>(
        data.gmst_float(std::string_view(name.utf8().get_data()), static_cast<float>(fallback)));
}

std::int64_t get_gmst_int(const WorldData& data, const String& name, std::int64_t fallback) {
    return data.gmst_int(std::string_view(name.utf8().get_data()), static_cast<std::int32_t>(fallback));
}

Dictionary get_weapon(const WorldData& data, std::int64_t form) {
    Dictionary out;
    const auto* base = data.base_ptr(form);
    const auto* item = base != nullptr ? base->item() : nullptr;
    if (item == nullptr || item->weapon_skill() < 0) {
        return out;
    }
    out["id"] = form;
    out["editor_id"] = to_godot(base->editor_id());
    out["name"] = to_godot(base->name());
    out["model"] = to_godot(base->model());
    out["weapon_type"] = static_cast<std::int64_t>(item->weapon_type());
    out["damage"] = static_cast<double>(item->damage());
    out["speed"] = static_cast<double>(item->speed());
    out["reach"] = static_cast<double>(item->reach());
    out["skill"] = static_cast<std::int64_t>(item->weapon_skill());
    out["stagger"] = static_cast<double>(item->stagger());
    out["impact_data_set"] = static_cast<std::int64_t>(item->impact_data_set());
    out["first_person_model"] = to_godot(item->first_person_model());
    out["equip_sound"] = static_cast<std::int64_t>(item->equip_sound());
    out["unequip_sound"] = static_cast<std::int64_t>(item->unequip_sound());
    return out;
}

Dictionary get_race(const WorldData& data, std::int64_t race_id) {
    Dictionary out;
    const auto* race = lookup(data.root() != nullptr ? data.root()->races() : nullptr, static_cast<std::uint32_t>(race_id));
    if (race == nullptr) {
        return out;
    }
    out["id"] = race_id;
    out["editor_id"] = to_godot(race->editor_id());
    out["unarmed_damage"] = static_cast<double>(race->unarmed_damage());
    out["unarmed_reach"] = static_cast<double>(race->unarmed_reach());
    out["impact_material"] = static_cast<std::int64_t>(race->impact_material());
    out["impact_data_set"] = static_cast<std::int64_t>(race->impact_data_set());
    Array behaviours;
    if (race->behaviours() != nullptr) {
        for (const auto* b : *race->behaviours()) {
            behaviours.push_back(to_godot(b));
        }
    }
    out["behaviours"] = behaviours;
    Array attacks;
    if (race->attacks() != nullptr) {
        for (const auto* a : *race->attacks()) {
            Dictionary d;
            d["event"] = to_godot(a->event());
            d["damage_mult"] = static_cast<double>(a->damage_mult());
            d["flags"] = static_cast<std::int64_t>(a->flags());
            d["attack_angle"] = static_cast<double>(a->attack_angle());
            d["strike_angle"] = static_cast<double>(a->strike_angle());
            d["stagger"] = static_cast<double>(a->stagger());
            attacks.push_back(d);
        }
    }
    out["attacks"] = attacks;
    return out;
}

Dictionary get_impact(const WorldData& data, std::int64_t data_set, std::int64_t material) {
    Dictionary out;
    const auto* root = data.root();
    if (root == nullptr) {
        return out;
    }
    const auto* set = lookup(root->impact_data_sets(), static_cast<std::uint32_t>(data_set));
    if (set == nullptr || set->entries() == nullptr) {
        return out;
    }
    std::uint32_t impact_id = 0;
    for (const auto* entry : *set->entries()) {
        if (entry->material() == static_cast<std::uint32_t>(material)) {
            impact_id = entry->impact();
            break;
        }
    }
    const auto* impact = impact_id != 0 ? lookup(root->impacts(), impact_id) : nullptr;
    if (impact == nullptr) {
        return out;
    }
    out["id"] = static_cast<std::int64_t>(impact_id);
    out["editor_id"] = to_godot(impact->editor_id());
    out["model"] = to_godot(impact->model());
    out["duration"] = static_cast<double>(impact->duration());
    out["orientation"] = static_cast<std::int64_t>(impact->orientation());
    out["sound1"] = static_cast<std::int64_t>(impact->sound1());
    out["sound2"] = static_cast<std::int64_t>(impact->sound2());
    return out;
}

} // namespace skydot::queries
