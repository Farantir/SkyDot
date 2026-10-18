// SPDX-License-Identifier: GPL-3.0-or-later
//
// Magic as world.fb 33 carries it: spells (SPEL), magic effects (MGEF),
// enchantments (ENCH), art objects (ARTO) and the effects of items.
#include "world/queries.hpp"
#include "data/fb_search.hpp"
#include "data/text.hpp"
#include "data/godot_values.hpp"

#include "world_generated.h"

using godot::Array;
using godot::Dictionary;
using godot::String;

namespace wfb = bethconv::pack::wfb;

namespace skydot::queries {
namespace {

std::int64_t whole(std::uint32_t v) { return static_cast<std::int64_t>(v); }

Dictionary condition_dictionary(const wfb::Condition& c) {
    Dictionary d;
    d["type"] = whole(c.type());
    d["function"] = whole(c.function());
    d["compare_to"] = static_cast<double>(c.value());
    d["value_global"] = whole(c.value_global());
    d["param1"] = whole(c.param1());
    d["param2"] = whole(c.param2());
    d["run_on"] = whole(c.run_on());
    d["reference"] = whole(c.reference());
    d["param3"] = static_cast<std::int64_t>(c.param3());
    return d;
}

Array conditions_array(const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list) {
    Array out;
    if (list != nullptr) {
        for (const auto* c : *list) {
            out.push_back(condition_dictionary(*c));
        }
    }
    return out;
}

/// {effect, magnitude, area, duration, conditions: [condition dictionaries]} per entry.
Array effect_array(const flatbuffers::Vector<flatbuffers::Offset<wfb::EffectItem>>* effects) {
    Array out;
    if (effects == nullptr) {
        return out;
    }
    for (const auto* e : *effects) {
        Dictionary d;
        d["effect"] = whole(e->effect());
        d["magnitude"] = static_cast<double>(e->magnitude());
        d["area"] = whole(e->area());
        d["duration"] = whole(e->duration());
        d["conditions"] = conditions_array(e->conditions());
        out.push_back(d);
    }
    return out;
}

} // namespace

Dictionary get_spell(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* s = lookup(data.root() != nullptr ? data.root()->spells() : nullptr,
                           static_cast<std::uint32_t>(id));
    if (s == nullptr) {
        return out;
    }
    out["id"] = id;
    out["editor_id"] = to_godot(s->editor_id());
    out["name"] = to_godot(s->name());
    out["cost"] = static_cast<std::int64_t>(s->cost());
    out["flags"] = whole(s->flags());
    out["type"] = whole(s->type());
    out["charge_time"] = static_cast<double>(s->charge_time());
    out["casting_type"] = whole(s->casting_type());
    out["delivery"] = whole(s->delivery());
    out["cast_duration"] = static_cast<double>(s->cast_duration());
    out["range"] = static_cast<double>(s->range());
    out["half_cost_perk"] = whole(s->half_cost_perk());
    out["equip_slot"] = whole(s->equip_slot());
    out["effects"] = effect_array(s->effects());
    return out;
}

Dictionary get_magic_effect(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* m = lookup(data.root() != nullptr ? data.root()->magic_effects() : nullptr,
                           static_cast<std::uint32_t>(id));
    if (m == nullptr) {
        return out;
    }
    const auto num = [](float v) { return static_cast<double>(v); };
    out["id"] = id;
    out["editor_id"] = to_godot(m->editor_id());
    out["name"] = to_godot(m->name());
    out["description"] = to_godot(m->description());
    out["flags"] = whole(m->flags());
    out["base_cost"] = num(m->base_cost());
    out["magic_skill"] = static_cast<std::int64_t>(m->magic_skill());
    out["actor_value"] = static_cast<std::int64_t>(m->actor_value());
    out["archetype"] = whole(m->archetype());
    out["casting_type"] = whole(m->casting_type());
    out["delivery"] = whole(m->delivery());
    out["associated_item"] = whole(m->associated_item());
    out["resist_value"] = static_cast<std::int64_t>(m->resist_value());
    out["second_actor_value"] = static_cast<std::int64_t>(m->second_actor_value());
    out["second_av_weight"] = num(m->second_av_weight());
    out["skill_level"] = whole(m->skill_level());
    out["area"] = whole(m->area());
    out["casting_time"] = num(m->casting_time());
    out["taper_weight"] = num(m->taper_weight());
    out["taper_curve"] = num(m->taper_curve());
    out["taper_duration"] = num(m->taper_duration());
    out["projectile"] = whole(m->projectile());
    out["explosion"] = whole(m->explosion());
    out["casting_light"] = whole(m->casting_light());
    out["hit_shader"] = whole(m->hit_shader());
    out["enchant_shader"] = whole(m->enchant_shader());
    out["casting_art"] = whole(m->casting_art());
    out["hit_effect_art"] = whole(m->hit_effect_art());
    out["enchant_art"] = whole(m->enchant_art());
    out["impact_data"] = whole(m->impact_data());
    out["skill_usage_multiplier"] = num(m->skill_usage_multiplier());
    out["dual_cast_art"] = whole(m->dual_cast_art());
    out["dual_cast_scale"] = num(m->dual_cast_scale());
    out["equip_ability"] = whole(m->equip_ability());
    out["image_space_mod"] = whole(m->image_space_mod());
    out["perk_to_apply"] = whole(m->perk_to_apply());
    out["casting_sound_level"] = whole(m->casting_sound_level());
    out["ai_score"] = num(m->ai_score());
    out["ai_delay_time"] = num(m->ai_delay_time());
    godot::PackedInt64Array keywords;
    if (m->keywords() != nullptr) {
        for (const auto k : *m->keywords()) {
            keywords.push_back(k);
        }
    }
    out["keywords"] = keywords;
    godot::PackedInt64Array counters;
    if (m->counter_effects() != nullptr) {
        for (const auto k : *m->counter_effects()) {
            counters.push_back(k);
        }
    }
    out["counter_effects"] = counters;
    Array sounds;
    if (m->sounds() != nullptr) {
        for (const auto* s : *m->sounds()) {
            Dictionary d;
            d["kind"] = whole(s->kind());
            d["sound"] = whole(s->sound());
            sounds.push_back(d);
        }
    }
    out["sounds"] = sounds;
    out["conditions"] = conditions_array(m->conditions());
    Array scripts;
    if (m->scripts() != nullptr) {
        for (const auto* s : *m->scripts()) {
            scripts.push_back(to_godot(s->name()));
        }
    }
    out["scripts"] = scripts;
    return out;
}

Dictionary get_enchantment(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* e = lookup(data.root() != nullptr ? data.root()->enchantments() : nullptr,
                           static_cast<std::uint32_t>(id));
    if (e == nullptr) {
        return out;
    }
    out["id"] = id;
    out["editor_id"] = to_godot(e->editor_id());
    out["name"] = to_godot(e->name());
    out["cost"] = static_cast<std::int64_t>(e->cost());
    out["charge"] = static_cast<std::int64_t>(e->charge());
    out["flags"] = whole(e->flags());
    out["casting_type"] = whole(e->casting_type());
    out["delivery"] = whole(e->delivery());
    out["enchantment_type"] = whole(e->enchantment_type());
    out["charge_time"] = static_cast<double>(e->charge_time());
    out["base_enchantment"] = whole(e->base_enchantment());
    out["worn_restrictions"] = whole(e->worn_restrictions());
    out["effects"] = effect_array(e->effects());
    return out;
}

Dictionary get_art_object(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* a = lookup(data.root() != nullptr ? data.root()->art_objects() : nullptr,
                           static_cast<std::uint32_t>(id));
    if (a == nullptr) {
        return out;
    }
    out["id"] = id;
    out["editor_id"] = to_godot(a->editor_id());
    out["model"] = to_godot(a->model());
    out["art_type"] = whole(a->art_type());
    return out;
}

Array get_item_effects(const WorldData& data, std::int64_t form) {
    const auto* base = data.base_ptr(form);
    const auto* item = base != nullptr ? base->item() : nullptr;
    return effect_array(item != nullptr ? item->effects() : nullptr);
}

} // namespace skydot::queries
