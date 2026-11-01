// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skills, perks and classes as world.fb 38 carries them: PERK, AVIF (skill use and improve values and
// the perk tree) and CLAS. The rules that use them are GDScript and C++ elsewhere.
#include "world/queries.hpp"
#include "data/fb_search.hpp"
#include "data/godot_values.hpp"
#include "data/text.hpp"

#include "world_generated.h"

#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>

using godot::Array;
using godot::Dictionary;

namespace wfb = bethconv::pack::wfb;

namespace skydot::queries {
namespace {

std::int64_t whole(std::uint32_t v) { return static_cast<std::int64_t>(v); }

Dictionary effect_dictionary(const wfb::PerkEffect& e) {
    Dictionary d;
    d["kind"] = static_cast<std::int64_t>(e.kind());
    d["rank"] = static_cast<std::int64_t>(e.rank());
    d["priority"] = static_cast<std::int64_t>(e.priority());
    d["quest"] = whole(e.quest());
    d["stage"] = static_cast<std::int64_t>(e.stage());
    d["spell"] = whole(e.spell());
    d["entry_point"] = static_cast<std::int64_t>(e.entry_point());
    d["function"] = static_cast<std::int64_t>(e.function());
    d["tab_count"] = static_cast<std::int64_t>(e.tab_count());
    Array tabs;
    if (e.tabs() != nullptr) {
        for (const auto* t : *e.tabs()) {
            Dictionary tab;
            tab["tab"] = static_cast<std::int64_t>(t->tab());
            tab["conditions"] = conditions_array(t->conditions());
            tabs.push_back(tab);
        }
    }
    d["tabs"] = tabs;
    d["data_type"] = static_cast<std::int64_t>(e.data_type());
    godot::PackedFloat64Array values;
    if (e.values() != nullptr) {
        for (const float v : *e.values()) {
            values.push_back(static_cast<double>(v));
        }
    }
    d["values"] = values;
    d["actor_value"] = static_cast<std::int64_t>(e.actor_value());
    d["form"] = whole(e.form());
    d["text"] = to_godot(e.text());
    d["script_flags"] = static_cast<std::int64_t>(e.script_flags());
    return d;
}

} // namespace

Dictionary get_perk(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* p = data.perk_ptr(static_cast<std::uint32_t>(id));
    if (p == nullptr) {
        return out;
    }
    out["id"] = id;
    out["editor_id"] = to_godot(p->editor_id());
    out["name"] = to_godot(p->name());
    out["description"] = to_godot(p->description());
    out["trait"] = p->trait();
    out["level"] = static_cast<std::int64_t>(p->level());
    out["ranks"] = static_cast<std::int64_t>(p->ranks());
    out["playable"] = p->playable();
    out["hidden"] = p->hidden();
    out["next_perk"] = whole(p->next_perk());
    out["conditions"] = conditions_array(p->conditions());
    Array effects;
    if (p->effects() != nullptr) {
        for (const auto* e : *p->effects()) {
            effects.push_back(effect_dictionary(*e));
        }
    }
    out["effects"] = effects;
    return out;
}

Dictionary get_actor_value_info(const WorldData& data, std::int64_t actor_value) {
    Dictionary out;
    const auto* a = data.actor_value_info_of(static_cast<std::int32_t>(actor_value));
    if (a == nullptr) {
        return out;
    }
    out["id"] = whole(a->id());
    out["editor_id"] = to_godot(a->editor_id());
    out["actor_value"] = static_cast<std::int64_t>(a->actor_value());
    out["name"] = to_godot(a->name());
    out["description"] = to_godot(a->description());
    out["abbreviation"] = to_godot(a->abbreviation());
    out["category"] = whole(a->category());
    out["has_skill"] = a->has_skill();
    out["use_mult"] = static_cast<double>(a->use_mult());
    out["use_offset"] = static_cast<double>(a->use_offset());
    out["improve_mult"] = static_cast<double>(a->improve_mult());
    out["improve_offset"] = static_cast<double>(a->improve_offset());
    Array tree;
    if (a->perk_tree() != nullptr) {
        for (const auto* n : *a->perk_tree()) {
            Dictionary d;
            d["index"] = whole(n->index());
            d["perk"] = whole(n->perk());
            d["flags"] = whole(n->flags());
            d["grid_x"] = static_cast<std::int64_t>(n->grid_x());
            d["grid_y"] = static_cast<std::int64_t>(n->grid_y());
            d["x"] = static_cast<double>(n->x());
            d["y"] = static_cast<double>(n->y());
            d["skill"] = whole(n->skill());
            godot::PackedInt64Array links;
            if (n->links() != nullptr) {
                for (const auto l : *n->links()) {
                    links.push_back(static_cast<std::int64_t>(l));
                }
            }
            d["links"] = links;
            tree.push_back(d);
        }
    }
    out["perk_tree"] = tree;
    return out;
}

Dictionary get_class(const WorldData& data, std::int64_t id) {
    Dictionary out;
    const auto* c = data.class_ptr(static_cast<std::uint32_t>(id));
    if (c == nullptr) {
        return out;
    }
    out["id"] = id;
    out["editor_id"] = to_godot(c->editor_id());
    out["name"] = to_godot(c->name());
    out["teaches"] = static_cast<std::int64_t>(c->teaches());
    out["max_training_level"] = static_cast<std::int64_t>(c->max_training_level());
    godot::PackedInt64Array weights;
    if (c->skill_weights() != nullptr) {
        for (const auto w : *c->skill_weights()) {
            weights.push_back(static_cast<std::int64_t>(w));
        }
    }
    out["skill_weights"] = weights;
    out["health_weight"] = static_cast<std::int64_t>(c->health_weight());
    out["magicka_weight"] = static_cast<std::int64_t>(c->magicka_weight());
    out["stamina_weight"] = static_cast<std::int64_t>(c->stamina_weight());
    out["bleedout_default"] = static_cast<double>(c->bleedout_default());
    out["voice_points"] = whole(c->voice_points());
    return out;
}

} // namespace skydot::queries
