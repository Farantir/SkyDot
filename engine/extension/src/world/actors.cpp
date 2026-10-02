// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/actors.hpp"

#include "world/fb_search.hpp"

#include <algorithm>

namespace skydot {
namespace {

namespace wfb = bethconv::pack::wfb;

constexpr std::uint16_t k_use_traits = 0x0001;
constexpr std::uint16_t k_use_inventory = 0x0100;
constexpr std::uint32_t k_female = 0x1;
/// Body slot 31 (bit 1): hair. Hoods and helmets cover it.
constexpr std::uint32_t k_slot_hair = 0x2;
/// Templates and leveled lists nest; vanilla stays far below this.
constexpr int k_max_depth = 8;
constexpr std::uint32_t k_lvln = 0x4E4C564C; // "LVLN"
/// LVLF: 4 use all, give every entry rather than one.
constexpr std::uint8_t k_use_all = 0x04;

const wfb::Npc* npc_of(const wfb::World& w, std::uint32_t id) {
    return find_sorted(w.npcs(), id, [](const wfb::Npc* n) { return n->id(); });
}
const wfb::Race* race_of(const wfb::World& w, std::uint32_t id) {
    return find_sorted(w.races(), id, [](const wfb::Race* r) { return r->id(); });
}
const wfb::Armor* armor_of(const wfb::World& w, std::uint32_t id) {
    return find_sorted(w.armors(), id, [](const wfb::Armor* a) { return a->id(); });
}
const wfb::ArmorAddon* addon_of(const wfb::World& w, std::uint32_t id) {
    return find_sorted(w.armor_addons(), id, [](const wfb::ArmorAddon* a) { return a->id(); });
}
const wfb::LeveledList* list_of(const wfb::World& w, std::uint32_t id) {
    return find_sorted(w.leveled_lists(), id, [](const wfb::LeveledList* l) { return l->id(); });
}
const wfb::Outfit* outfit_of(const wfb::World& w, std::uint32_t id) {
    return find_sorted(w.outfits(), id, [](const wfb::Outfit* o) { return o->id(); });
}

/// One entry of a leveled list, the same every time for this reference:
/// among the entries at or below `level` (all if `level` is 0 or none
/// qualify), chosen by a hash of the reference.
std::uint32_t pick(const wfb::LeveledList& list, std::uint32_t ref, std::uint16_t level = 0) {
    const auto* entries = list.entries();
    if (entries == nullptr || entries->size() == 0) {
        return 0;
    }
    std::vector<std::uint32_t> eligible;
    for (const auto* e : *entries) {
        if (level == 0 || e->level() <= level) {
            eligible.push_back(e->form());
        }
    }
    if (eligible.empty()) {
        for (const auto* e : *entries) {
            eligible.push_back(e->form());
        }
    }
    std::uint32_t h = (ref ^ list.id()) * 2654435761u;
    h ^= h >> 15;
    return eligible[h % eligible.size()];
}

/// Follow TPLT while the NPC takes `flag` from its template.
const wfb::Npc* resolve(const wfb::World& w, const wfb::Npc* npc, std::uint16_t flag, std::uint32_t ref) {
    for (int depth = 0; npc != nullptr && depth < k_max_depth; ++depth) {
        if ((npc->template_flags() & flag) == 0 || npc->template_() == 0) {
            return npc;
        }
        std::uint32_t next = npc->template_();
        for (int inner = 0; inner < k_max_depth; ++inner) {
            const auto* list = list_of(w, next);
            if (list == nullptr || list->type() != k_lvln) {
                break;
            }
            next = pick(*list, ref);
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

/// The armor an outfit item stands for: itself, every entry of a "use all"
/// leveled list, or one pick from another.
void worn_items(const wfb::World& w, std::uint32_t id, std::uint32_t ref, std::uint16_t level,
                std::vector<const wfb::Armor*>& out, int depth = 0) {
    if (depth >= k_max_depth) {
        return;
    }
    if (const auto* armor = armor_of(w, id)) {
        out.push_back(armor);
        return;
    }
    const auto* list = list_of(w, id);
    if (list == nullptr || list->entries() == nullptr) {
        return;
    }
    if ((list->flags() & k_use_all) != 0) {
        for (const auto* e : *list->entries()) {
            worn_items(w, e->form(), ref, level, out, depth + 1);
        }
        return;
    }
    worn_items(w, pick(*list, ref, level), ref, level, out, depth + 1);
}

/// The addon's model for this sex and weight, if the pack has it.
std::string addon_model(const wfb::ArmorAddon& addon, bool female, float weight,
                        const std::function<bool(const std::string&)>& exists) {
    std::string path = str(female ? addon.female_model() : addon.male_model());
    if (path.empty()) {
        path = str(female ? addon.male_model() : addon.female_model());
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

const wfb::Npc* resolve_npc(const wfb::World& w, std::uint32_t npc, std::uint16_t flag, std::uint32_t ref) {
    return resolve(w, npc_of(w, npc), flag, ref);
}

ActorPlan plan_actor(const wfb::World& w, std::uint32_t npc_id, std::uint32_t ref,
                     const std::function<bool(const std::string&)>& exists) {
    ActorPlan plan;
    // The placed base may itself be a leveled list.
    std::uint32_t base = npc_id;
    for (int depth = 0; depth < k_max_depth; ++depth) {
        const auto* list = list_of(w, base);
        if (list == nullptr) {
            break;
        }
        base = pick(*list, ref);
    }
    const auto* npc = npc_of(w, base);
    if (npc == nullptr) {
        plan.missing = "no NPC_";
        return plan;
    }
    const auto* traits = resolve(w, npc, k_use_traits, ref);
    const auto* inventory = resolve(w, npc, k_use_inventory, ref);
    plan.npc = traits->id();
    if (const auto* tone = traits->skin_tone(); tone != nullptr && tone->size() == 3) {
        for (flatbuffers::uoffset_t i = 0; i < 3; ++i) {
            plan.skin_tone[i] = tone->Get(i);
        }
    }
    plan.female = (traits->flags() & k_female) != 0;
    const std::size_t sex = plan.female ? 1 : 0;
    const auto* race = race_of(w, traits->race());
    if (race == nullptr) {
        plan.missing = "no RACE";
        return plan;
    }
    plan.race = race->id();
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
            if (skin && (addon->slots() & covered) != 0) {
                continue;
            }
            const std::string model = addon_model(*addon, plan.female, traits->weight(), exists);
            if (!model.empty() && std::find(plan.parts.begin(), plan.parts.end(), model) == plan.parts.end()) {
                plan.parts.push_back(model);
            }
        }
    };
    if (const auto* outfit = outfit_of(w, inventory->default_outfit()); outfit != nullptr && outfit->items() != nullptr) {
        std::vector<const wfb::Armor*> items;
        for (const std::uint32_t item : *outfit->items()) {
            worn_items(w, item, ref, npc->level(), items);
        }
        std::vector<const wfb::Armor*> worn;
        for (const auto* armor : items) {
            // Two items on one slot: the first keeps it.
            if ((armor->slots() & covered) != 0) {
                continue;
            }
            covered |= armor->slots();
            worn.push_back(armor);
        }
        for (const auto* armor : worn) {
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
