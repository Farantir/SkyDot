// SPDX-License-Identifier: GPL-3.0-or-later
#include "data/actors.hpp"

#include "data/fb_search.hpp"
#include "data/leveled.hpp"

#include "skydot_formats/flags.hpp"

#include <algorithm>

namespace skydot {
namespace {

namespace wfb = bethconv::pack::wfb;

/// Body slot 31 (bit 1): hair. Hoods and helmets cover it.
constexpr std::uint32_t k_slot_hair = 0x2;
constexpr std::uint32_t k_slot_body = 0x4;
/// Templates and leveled lists nest; vanilla stays far below this.
constexpr int k_max_depth = 8;
constexpr std::uint32_t k_lvln = 0x4E4C564C; // "LVLN"

const wfb::Npc* npc_of(const wfb::World& w, std::uint32_t id) {
    return lookup(w.npcs(), id);
}
const wfb::Race* race_of(const wfb::World& w, std::uint32_t id) {
    return lookup(w.races(), id);
}
const wfb::Armor* armor_of(const wfb::World& w, std::uint32_t id) {
    return lookup(w.armors(), id);
}
const wfb::ArmorAddon* addon_of(const wfb::World& w, std::uint32_t id) {
    return lookup(w.armor_addons(), id);
}
const wfb::LeveledList* list_of(const wfb::World& w, std::uint32_t id) {
    return lookup(w.leveled_lists(), id);
}
const wfb::Outfit* outfit_of(const wfb::World& w, std::uint32_t id) {
    return lookup(w.outfits(), id);
}

/// The actor a leveled list stands for, the same every time for this
/// reference (leveled.hpp); `id` itself if it is no list.
std::uint32_t pick_actor(const wfb::World& w, std::uint32_t id, std::uint32_t ref) {
    leveled::Rng rng(leveled::seed_for(ref, leveled::k_purpose_actor));
    return leveled::pick_one(w, id, leveled::player_level(), rng);
}

/// Follow TPLT while the NPC takes `flag` from its template.
const wfb::Npc* resolve(const wfb::World& w, const wfb::Npc* npc, wfb::NpcTemplateFlags flag,
                        std::uint32_t ref) {
    for (int depth = 0; npc != nullptr && depth < k_max_depth; ++depth) {
        if (!formats::has_flag(npc->template_flags(), flag) || npc->template_() == 0) {
            return npc;
        }
        std::uint32_t next = npc->template_();
        if (const auto* list = list_of(w, next); list != nullptr && list->type() == k_lvln) {
            next = pick_actor(w, next, ref);
        }
        const auto* t = npc_of(w, next);
        if (t == nullptr) {
            return npc;
        }
        npc = t;
    }
    return npc;
}

std::string str(const flatbuffers::String* s) {
    return s == nullptr ? std::string() : s->str();
}

std::string at(const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>* v, std::size_t i) {
    return v != nullptr && i < v->size() ? v->Get(static_cast<flatbuffers::uoffset_t>(i))->str() : std::string();
}

bool contains(const flatbuffers::Vector<std::uint32_t>* v, std::uint32_t id) {
    if (v == nullptr || id == 0) {
        return false;
    }
    return std::find(v->begin(), v->end(), id) != v->end();
}

bool fits(const wfb::ArmorAddon& addon, const wfb::Race& race) {
    const std::uint32_t r = race.id();
    const std::uint32_t armor_race = race.armor_race();
    return addon.race() == r || contains(addon.additional_races(), r) ||
           (armor_race != 0 && (addon.race() == armor_race || contains(addon.additional_races(), armor_race)));
}

/// The armor an outfit item stands for: itself, or what a leveled list
/// gives (leveled.hpp, drawing from `rng`).
void worn_items(const wfb::World& w, std::uint32_t id, std::uint16_t level, leveled::Rng& rng,
                std::vector<const wfb::Armor*>& out) {
    if (const auto* armor = armor_of(w, id)) {
        out.push_back(armor);
        return;
    }
    for (const auto& entry : leveled::resolve(w, id, level, rng)) {
        if (const auto* armor = armor_of(w, entry.form)) {
            out.push_back(armor);
        }
    }
}

/// The addon's model for this sex and weight, if the pack has it.
std::string addon_model(const wfb::ArmorAddon& addon, bool female, float weight,
                        const std::function<bool(const std::string&)>& exists, bool first_person = false) {
    std::string path = first_person ? str(female ? addon.female_model_1st() : addon.male_model_1st())
                                    : str(female ? addon.female_model() : addon.male_model());
    if (path.empty()) {
        path = first_person ? str(female ? addon.male_model_1st() : addon.female_model_1st())
                            : str(female ? addon.male_model() : addon.female_model());
    }
    if (path.empty()) {
        return {};
    }
    const std::uint8_t slider = female ? addon.female_weight_slider() : addon.male_weight_slider();
    if ((slider & 2) != 0 && path.size() > 6 &&
        (path.ends_with("_0.nif") || path.ends_with("_1.nif"))) {
        std::string variant = path;
        variant[variant.size() - 5] = weight >= 50.0F ? '1' : '0';
        if (exists(variant)) {
            return variant;
        }
    }
    return exists(path) ? path : std::string();
}

std::string with_extension(const std::string& path, const char* ext) {
    const auto dot = path.rfind('.');
    return dot == std::string::npos ? path : path.substr(0, dot) + ext;
}

} // namespace

std::uint32_t pick_actor_form(const wfb::World& w, std::uint32_t id, std::uint32_t ref) {
    return pick_actor(w, id, ref);
}

std::uint16_t actor_level(const wfb::Npc& npc) {
    if (!formats::has_flag(npc.flags(), wfb::NpcFlags::pc_level_mult)) {
        return std::max<std::uint16_t>(npc.level(), 1);
    }
    // The level word is a multiplier of the player's level, in thousandths.
    auto level = static_cast<std::uint32_t>(leveled::player_level()) * npc.level() / 1000;
    level = std::max(level, std::max<std::uint32_t>(npc.calc_min_level(), 1));
    if (npc.calc_max_level() != 0) {
        level = std::min<std::uint32_t>(level, npc.calc_max_level());
    }
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(level, 0xFFFF));
}

const wfb::Npc* resolve_npc(const wfb::World& w, std::uint32_t npc, wfb::NpcTemplateFlags flag,
                            std::uint32_t ref) {
    return resolve(w, npc_of(w, npc), flag, ref);
}

bool actor_base_values(const wfb::World& w, std::uint32_t npc_id, std::uint32_t ref, vm::ActorBase& out) {
    const auto* npc = npc_of(w, pick_actor(w, npc_id, ref));
    if (npc == nullptr) {
        return false;
    }
    const auto* traits = resolve(w, npc, wfb::NpcTemplateFlags::use_traits, ref);
    const auto* stats = resolve(w, npc, wfb::NpcTemplateFlags::use_stats, ref);
    const auto* ai = resolve(w, npc, wfb::NpcTemplateFlags::use_ai_data, ref);
    const auto* race = race_of(w, traits->race());
    if (race == nullptr) {
        return false;
    }
    out = vm::ActorBase{};
    if (const auto* placed = lookup(w.actors(), ref); placed != nullptr) {
        out.starts_dead = formats::has_flag(placed->flags(), wfb::RefFlags::starts_dead);
    }
    auto& base = out.base;
    out.essential = formats::has_flag(npc->flags(), wfb::NpcFlags::essential);
    // Health, Magicka and Stamina. An NPC with auto-calculated stats stores
    // the CK's total in DNAM, race start included (the player: race 50, offset
    // 50, DNAM 100, and 100 in the game; Addvar 91, a mud crab 5 with offset
    // -20 on a race of 25). Any other NPC has a DNAM that is not a total
    // (57558 on some), so it is the race's start plus the ACBS offset.
    if (formats::has_flag(stats->flags(), wfb::NpcFlags::auto_calc_stats)) {
        base[vm::av::k_health] = stats->health();
        base[vm::av::k_magicka] = stats->magicka();
        base[vm::av::k_stamina] = stats->stamina();
    } else {
        base[vm::av::k_health] = static_cast<double>(race->start_health()) + stats->health_offset();
        base[vm::av::k_magicka] = static_cast<double>(race->start_magicka()) + stats->magicka_offset();
        base[vm::av::k_stamina] = static_cast<double>(race->start_stamina()) + stats->stamina_offset();
    }
    // Skills: DNAM value plus offset. Auto-calculated totals already hold the
    // race's boosts (the player: Nord One-handed 15 + 5, Two-handed 15 + 10);
    // for the others they are added (unverified against the original).
    const bool totals = formats::has_flag(stats->flags(), wfb::NpcFlags::auto_calc_stats);
    for (std::uint32_t i = 0; i < vm::av::k_skill_count; ++i) {
        double value = 0.0;
        if (stats->skill_values() != nullptr && i < stats->skill_values()->size()) {
            value += stats->skill_values()->Get(i);
        }
        if (stats->skill_offsets() != nullptr && i < stats->skill_offsets()->size()) {
            value += stats->skill_offsets()->Get(i);
        }
        base[vm::av::k_first_skill + i] = value;
    }
    if (!totals && race->skill_boosts() != nullptr) {
        const auto& boosts = *race->skill_boosts();
        for (flatbuffers::uoffset_t i = 0; i + 1 < boosts.size(); i += 2) {
            const std::uint32_t skill = boosts.Get(i);
            if (skill >= vm::av::k_first_skill && skill < vm::av::k_first_skill + vm::av::k_skill_count) {
                base[skill] += boosts.Get(i + 1);
            }
        }
    }
    base[vm::av::k_heal_rate] = static_cast<double>(race->health_regen());
    base[vm::av::k_magicka_rate] = static_cast<double>(race->magicka_regen());
    base[vm::av::k_stamina_rate] = static_cast<double>(race->stamina_regen());
    base[vm::av::k_heal_rate_mult] = 100.0;
    base[vm::av::k_magicka_rate_mult] = 100.0;
    base[vm::av::k_stamina_rate_mult] = 100.0;
    base[vm::av::k_shout_recovery_mult] = 1.0;
    base[vm::av::k_speed_mult] = static_cast<double>(stats->speed_multiplier());
    base[vm::av::k_carry_weight] = static_cast<double>(race->carry_weight());
    base[0] = ai->aggression();
    base[1] = ai->confidence();
    base[2] = ai->energy();
    base[3] = ai->morality();
    base[4] = ai->mood();
    base[5] = ai->assistance();
    return true;
}

ActorPlan plan_actor(const wfb::World& w, std::uint32_t npc_id, std::uint32_t ref,
                     const std::function<bool(const std::string&)>& exists, const WornProvider& worn) {
    ActorPlan plan;
    // The placed base may itself be a leveled list.
    const auto* npc = npc_of(w, pick_actor(w, npc_id, ref));
    if (npc == nullptr) {
        plan.missing = "no NPC_";
        return plan;
    }
    const auto* traits = resolve(w, npc, wfb::NpcTemplateFlags::use_traits, ref);
    const auto* inventory = resolve(w, npc, wfb::NpcTemplateFlags::use_inventory, ref);
    plan.npc = traits->id();
    if (const auto* tone = traits->skin_tone(); tone != nullptr && tone->size() == 3) {
        for (flatbuffers::uoffset_t i = 0; i < 3; ++i) {
            plan.skin_tone[i] = tone->Get(i);
        }
    }
    plan.female = formats::has_flag(traits->flags(), wfb::NpcFlags::female);
    const std::size_t sex = plan.female ? 1 : 0;
    const auto* race = race_of(w, traits->race());
    if (race == nullptr) {
        plan.missing = "no RACE";
        return plan;
    }
    plan.race = race->id();
    if (const auto* movement = lookup(w.movement_types(), race->movement_walk()); movement != nullptr &&
                                                                                 movement->rotate_in_place_walk() > 0.0F) {
        plan.turn_rate = movement->rotate_in_place_walk();
    }
    const float npc_height = traits->height() > 0.0F ? traits->height() : 1.0F;
    float race_height = 1.0F;
    if (const auto* h = race->heights(); h != nullptr && sex < h->size() && h->Get(static_cast<flatbuffers::uoffset_t>(sex)) > 0.0F) {
        race_height = h->Get(static_cast<flatbuffers::uoffset_t>(sex));
    }
    plan.scale = npc_height * race_height;

    std::string skeleton = at(race->skeletons(), sex);
    if (skeleton.empty()) {
        skeleton = at(race->skeletons(), 1 - sex);
    }
    plan.skeleton = with_extension(skeleton, ".hkx");
    if (plan.skeleton.empty() || !exists(plan.skeleton)) {
        plan.missing = "no animation skeleton beside " + skeleton;
        plan.skeleton.clear();
        return plan;
    }

    // Worn armor, then the skin where nothing covers it.
    std::uint32_t covered = 0;
    const auto add_armor = [&](const wfb::Armor& armor, bool skin) {
        const auto* addons = armor.addons();
        if (addons == nullptr) {
            return;
        }
        for (const std::uint32_t id : *addons) {
            const auto* addon = addon_of(w, id);
            if (addon == nullptr || !fits(*addon, *race)) {
                continue;
            }
            // The skin stays where armor leaves part of it bare: a draugr's one skin addon holds head, body
            // and hands (slots 30, 32, 33), and a helmet (slot 30) must not take the whole body with it.
            // It goes when the body slot or all of its slots are covered.
            if (skin && addon->slots() != 0 &&
                ((addon->slots() & covered & k_slot_body) != 0 || (addon->slots() & ~covered) == 0)) {
                continue;
            }
            const std::string model = addon_model(*addon, plan.female, traits->weight(), exists);
            if (!model.empty() && std::find(plan.parts.begin(), plan.parts.end(), model) == plan.parts.end()) {
                plan.parts.push_back(model);
            }
            const std::string model_1st = addon_model(*addon, plan.female, traits->weight(), exists, true);
            if (!model_1st.empty() &&
                std::find(plan.parts_1st.begin(), plan.parts_1st.end(), model_1st) == plan.parts_1st.end()) {
                plan.parts_1st.push_back(model_1st);
            }
        }
    };
    // What the actor wears: as the game knows it (the VM's equipment), else
    // its default outfit, drawn as the VM draws it.
    std::vector<const wfb::Armor*> items;
    std::vector<std::uint32_t> provided;
    bool known = false;
    if (worn && worn(ref, provided)) {
        known = true;
        for (const std::uint32_t id : provided) {
            if (const auto* armor = armor_of(w, id)) {
                items.push_back(armor);
            }
        }
    } else if (const auto* outfit = outfit_of(w, inventory->default_outfit());
               outfit != nullptr && outfit->items() != nullptr) {
        known = true;
        const auto& outfit_items = *outfit->items();
        leveled::Rng rng(leveled::seed_for(ref, leveled::k_purpose_outfit));
        for (const std::uint32_t item : outfit_items) {
            worn_items(w, item, actor_level(*npc), rng, items);
        }
    }
    if (known) {
        std::vector<const wfb::Armor*> kept;
        for (const auto* armor : items) {
            // Two items on one slot: the first keeps it.
            if ((armor->slots() & covered) != 0) {
                continue;
            }
            covered |= armor->slots();
            kept.push_back(armor);
        }
        for (const auto* armor : kept) {
            add_armor(*armor, false);
        }
        plan.hide_hair = (covered & k_slot_hair) != 0;
    }
    const std::uint32_t skin_id = traits->skin() != 0 ? traits->skin() : race->skin();
    if (const auto* skin = armor_of(w, skin_id)) {
        add_armor(*skin, true);
    }
    // Head: the precomputed FaceGen NIF. It includes the hair, which items
    // covering slot 31 hide (`hide_hair`).
    const std::string face = str(traits->face_model());
    if (!face.empty() && exists(face)) {
        plan.parts.push_back(face);
    }

    const std::string behaviour = at(race->behaviours(), sex);
    plan.behaviour = behaviour;
    const auto slash = behaviour.rfind('/');
    if (slash != std::string::npos) {
        const std::string dir = behaviour.substr(0, slash + 1) + "animations/";
        for (const std::string& name :
             {dir + (plan.female ? "female/" : "male/") + "mt_idle.hkx", dir + "mt_idle.hkx", dir + "idle.hkx",
              dir + "idle1.hkx", dir + "idlestand.hkx"}) {
            if (exists(name)) {
                plan.idle = name;
                break;
            }
        }
    }
    return plan;
}

} // namespace skydot
