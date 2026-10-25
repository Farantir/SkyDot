// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/form_magic.hpp"

#include "skydot_formats/vpath.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bethconv::cli {
namespace {

namespace wfb = bethconv::pack::wfb;

/// UESP names of the MGEF archetypes (DATA offset 64).
constexpr std::array<std::string_view, 47> k_archetype_names{
    "ValueModifier", "Script", "Dispel", "CureDisease", "Absorb", "DualValueModifier", "Calm",
    "Demoralize", "Frenzy", "Disarm", "CommandSummoned", "Invisibility", "Light", "Unknown13",
    "Unknown14", "Lock", "Open", "BoundWeapon", "SummonCreature", "DetectLife", "Telekinesis",
    "Paralysis", "Reanimate", "SoulTrap", "TurnUndead", "Guide", "WerewolfFeed", "CureParalysis",
    "CureAddiction", "CurePoison", "Concussion", "ValueAndParts", "AccumulateMagnitude", "Stagger",
    "PeakValueModifier", "Cloak", "Werewolf", "SlowTime", "Rally", "EnhanceWeapon", "SpawnHazard",
    "Etherealize", "Banish", "SpawnScriptedRef", "Disguise", "GrabActor", "VampireLord"};

std::string archetype_name(std::uint32_t archetype) {
    return archetype < k_archetype_names.size() ? std::string(k_archetype_names[archetype])
                                                : "Unknown" + std::to_string(archetype);
}

enum Source : std::size_t { spel, alch, scrl, ench, ingr, other, k_sources };

/// One effect entry's use: where it is and which MGEF.
struct Use {
    Source source;
    std::uint32_t form;
    std::uint32_t mgef;
};

struct Census {
    std::vector<Use> uses;
    /// Forms already counted, so a spell shared by 40 actors is counted once.
    std::set<std::uint32_t> seen;
};

class Collector {
public:
    explicit Collector(const pack::WorldFile& world) : world_(world) {}

    void add_effects(Census& c, Source source, std::uint32_t form,
                     const std::vector<std::unique_ptr<wfb::EffectItemT>>& effects) {
        if (!c.seen.insert(form).second) {
            return;
        }
        ++forms_[&c][source];
        for (const auto& e : effects) {
            c.uses.push_back({source, form, e->effect});
        }
    }

    void add_spell(Census& c, std::uint32_t id, std::set<std::uint32_t>& visited) {
        if (!visited.insert(id).second) {
            return;
        }
        if (const auto spell = world_.spell(id)) {
            add_effects(c, spel, id, spell->effects);
        } else if (const auto list = world_.leveled_list(id)) {
            for (const auto& e : list->entries) {
                add_spell(c, e.form(), visited);
            }
        } else if (world_.shout(id)) {
            slice_shouts_.insert(id);
        } else {
            ++unknown_spell_forms_;
            unknown_spell_ids_.insert(id);
        }
    }

    void add_item(Census& c, std::uint32_t id, std::set<std::uint32_t>& visited) {
        if (!visited.insert(id).second) {
            return;
        }
        if (const auto list = world_.leveled_list(id)) {
            for (const auto& e : list->entries) {
                add_item(c, e.form(), visited);
            }
            return;
        }
        const auto base = world_.base(id);
        if (!base || !base->item) {
            return;
        }
        const auto& item = *base->item;
        if (!item.effects.empty()) {
            Source s = other;
            switch (item.category) {
            case wfb::ItemCategory::potion: case wfb::ItemCategory::food: s = alch; break;
            case wfb::ItemCategory::scroll: s = scrl; break;
            case wfb::ItemCategory::ingredient: s = ingr; break;
            default: break;
            }
            add_effects(c, s, id, item.effects);
        }
        if (item.enchantment != 0 && !c.seen.contains(item.enchantment)) {
            if (const auto e = world_.enchantment(item.enchantment)) {
                add_effects(c, ench, item.enchantment, e->effects);
            }
        }
    }

    [[nodiscard]] std::size_t forms(const Census& c, Source s) const {
        const auto it = forms_.find(&c);
        return it == forms_.end() ? 0 : it->second[s];
    }
    [[nodiscard]] std::size_t unknown_spell_forms() const { return unknown_spell_forms_; }
    /// The distinct SHOUs the spell lists given to add_spell name.
    [[nodiscard]] const std::set<std::uint32_t>& shouts() const { return slice_shouts_; }

private:
    const pack::WorldFile& world_;
    mutable std::map<const Census*, std::array<std::size_t, k_sources>> forms_;
    std::size_t unknown_spell_forms_{};
    std::set<std::uint32_t> unknown_spell_ids_;
    std::set<std::uint32_t> slice_shouts_;
};

bool starts_with_any(const std::string& text, std::initializer_list<std::string_view> prefixes) {
    const std::string lower = skydot::formats::ascii_lower(text);
    return std::ranges::any_of(prefixes, [&](std::string_view p) {
        return lower.starts_with(skydot::formats::ascii_lower(std::string(p)));
    });
}

} // namespace

void print_magic_census(const pack::WorldFile& world) {
    Collector collector(world);

    // ---- the whole world ------------------------------------------------
    Census whole;
    for (const auto& spell : world.spells()) {
        collector.add_effects(whole, spel, spell.id, spell.effects);
    }
    for (const auto& e : world.enchantments()) {
        collector.add_effects(whole, ench, e.id, e.effects);
    }
    for (const auto& base : world.item_bases()) {
        if (!base.item || base.item->effects.empty()) {
            continue;
        }
        Source s = other;
        switch (base.item->category) {
        case wfb::ItemCategory::potion: case wfb::ItemCategory::food: s = alch; break;
        case wfb::ItemCategory::scroll: s = scrl; break;
        case wfb::ItemCategory::ingredient: s = ingr; break;
        default: break;
        }
        collector.add_effects(whole, s, base.id, base.item->effects);
    }

    // ---- the slice -------------------------------------------------------
    std::unordered_set<std::uint32_t> worlds;
    for (const auto& w : world.worldspaces()) {
        const std::string name = skydot::formats::ascii_lower(w.editor_id);
        if (name == "whiterunworld" || name == "tamriel") {
            worlds.insert(w.id);
        }
    }
    std::unordered_set<std::uint32_t> slice_cells;
    std::vector<std::uint32_t> slice_ref_bases;
    std::size_t interior_cells = 0;
    std::size_t exterior_cells = 0;
    for (const auto& cell : world.cell_summaries()) {
        bool in = false;
        if (cell.interior) {
            in = starts_with_any(cell.editor_id,
                                 {"Riverwood", "BleakFalls", "Whiterun", "Dragonsreach", "Helgen"});
            interior_cells += in ? 1 : 0;
        } else if (cell.has_grid && worlds.contains(cell.world)) {
            in = cell.grid_x >= -3 && cell.grid_x <= 10 && cell.grid_y >= -22 && cell.grid_y <= 2;
            exterior_cells += in ? 1 : 0;
        }
        if (in) {
            slice_cells.insert(cell.id);
            slice_ref_bases.insert(slice_ref_bases.end(), cell.ref_bases.begin(),
                                   cell.ref_bases.end());
        }
    }

    Census slice;
    std::set<std::uint32_t> visited_spells;
    std::set<std::uint32_t> visited_items;
    std::set<std::uint32_t> npcs;
    std::size_t placed = 0;
    // Actor bases to NPC_ ids; an LVLN expands to everything it names.
    std::set<std::uint32_t> visited_lists;
    const auto expand_actor = [&](auto&& self, std::uint32_t id) -> void {
        if (const auto list = world.leveled_list(id)) {
            if (visited_lists.insert(id).second) {
                for (const auto& e : list->entries) {
                    self(self, e.form());
                }
            }
            return;
        }
        npcs.insert(id);
    };
    for (const auto& a : world.actors()) {
        if (slice_cells.contains(a.cell())) {
            ++placed;
            expand_actor(expand_actor, a.base());
        }
    }
    npcs.insert(0x7); // the player
    const std::size_t npc_count = npcs.size();

    std::set<std::uint32_t> races;
    const auto add_npc_spells = [&](auto&& self, const wfb::NpcT& npc, int depth) -> void {
        for (const auto s : npc.spells) {
            collector.add_spell(slice, s, visited_spells);
        }
        if (depth < 4 && npc.template_ != 0 && (static_cast<std::uint32_t>(npc.template_flags) & 0x8U) != 0) {
            std::set<std::uint32_t> ids;
            std::set<std::uint32_t> lists;
            const auto resolve = [&](auto&& r, std::uint32_t id) -> void {
                if (const auto list = world.leveled_list(id)) {
                    if (lists.insert(id).second) {
                        for (const auto& e : list->entries) {
                            r(r, e.form());
                        }
                    }
                } else {
                    ids.insert(id);
                }
            };
            resolve(resolve, npc.template_);
            for (const auto t : ids) {
                if (const auto tn = world.npc(t)) {
                    self(self, *tn, depth + 1);
                }
            }
        }
    };
    std::size_t missing_npcs = 0;
    for (const auto id : npcs) {
        const auto npc = world.npc(id);
        if (!npc) {
            ++missing_npcs;
            continue;
        }
        races.insert(npc->race);
        add_npc_spells(add_npc_spells, *npc, 0);
        for (const auto& item : npc->items) {
            collector.add_item(slice, item.form(), visited_items);
        }
    }
    for (const auto id : races) {
        if (const auto race = world.race(id)) {
            for (const auto s : race->spells) {
                collector.add_spell(slice, s, visited_spells);
            }
        }
    }
    std::set<std::uint32_t> container_bases;
    for (const auto b : slice_ref_bases) {
        container_bases.insert(b);
    }
    std::size_t containers = 0;
    for (const auto b : container_bases) {
        const auto base = world.base(b);
        if (!base || base->items.empty()) {
            continue;
        }
        ++containers;
        for (const auto& item : base->items) {
            collector.add_item(slice, item.form(), visited_items);
        }
    }

    // ---- the table ---------------------------------------------------------
    struct Row {
        std::size_t slice_uses{};
        std::set<std::uint32_t> slice_mgefs;
        std::size_t world_uses{};
        std::set<std::uint32_t> world_mgefs;
        std::array<std::size_t, k_sources> world_by_source{};
    };
    std::map<std::uint32_t, Row> rows;
    std::unordered_map<std::uint32_t, std::uint32_t> archetype_of;
    std::size_t missing_mgef = 0;
    const auto archetype = [&](std::uint32_t mgef) -> std::optional<std::uint32_t> {
        if (const auto it = archetype_of.find(mgef); it != archetype_of.end()) {
            return it->second;
        }
        const auto m = world.magic_effect(mgef);
        if (!m) {
            return std::nullopt;
        }
        archetype_of[mgef] = m->archetype;
        return m->archetype;
    };
    for (const auto& u : whole.uses) {
        const auto a = archetype(u.mgef);
        if (!a) {
            ++missing_mgef;
            continue;
        }
        auto& row = rows[*a];
        ++row.world_uses;
        row.world_mgefs.insert(u.mgef);
        ++row.world_by_source[u.source];
    }
    for (const auto& u : slice.uses) {
        if (const auto a = archetype(u.mgef)) {
            auto& row = rows[*a];
            ++row.slice_uses;
            row.slice_mgefs.insert(u.mgef);
        }
    }
    std::vector<std::pair<std::uint32_t, Row>> sorted(rows.begin(), rows.end());
    std::ranges::stable_sort(sorted, [](const auto& a, const auto& b) {
        return a.second.slice_uses > b.second.slice_uses;
    });

    std::printf("magic census (effect entries of distinct SPEL/ALCH/SCRL/ENCH/INGR forms)\n");
    std::printf("whole world: %zu SPEL, %zu ALCH, %zu SCRL, %zu ENCH, %zu INGR, %zu other items "
                "with effects; %zu effect entries\n",
                collector.forms(whole, spel), collector.forms(whole, alch),
                collector.forms(whole, scrl), collector.forms(whole, ench),
                collector.forms(whole, ingr), collector.forms(whole, other), whole.uses.size());
    std::printf("slice: %zu interior cells, %zu exterior cells, %zu placed actors, %zu distinct "
                "actor bases (with the player), %zu races, %zu container bases with items\n",
                interior_cells, exterior_cells, placed, npc_count, races.size(), containers);
    std::printf("slice sources: %zu SPEL, %zu ALCH, %zu SCRL, %zu ENCH, %zu INGR, %zu other; %zu "
                "effect entries\n",
                collector.forms(slice, spel), collector.forms(slice, alch),
                collector.forms(slice, scrl), collector.forms(slice, ench),
                collector.forms(slice, ingr), collector.forms(slice, other), slice.uses.size());
    {
        std::vector<std::string> names;
        for (const auto id : collector.shouts()) {
            const auto shout = world.shout(id);
            names.push_back(shout ? shout->editor_id : std::string{});
        }
        std::ranges::sort(names);
        std::string list;
        for (const auto& name : names) {
            list += (list.empty() ? "" : ", ") + name;
        }
        std::printf("slice shouts: %zu SHOU (%s); whole world: %zu SHOU\n",
                    names.size(), list.c_str(), world.shouts().size());
    }
    if (collector.unknown_spell_forms() != 0 || missing_npcs != 0 || missing_mgef != 0) {
        std::printf("not resolved: %zu spell-list forms that are neither SPEL, LVSP nor SHOU, "
                    "%zu actor bases missing, %zu effects naming no MGEF\n",
                    collector.unknown_spell_forms(), missing_npcs, missing_mgef);
    }
    std::printf("\n%4s  %-20s %9s %6s  %9s %6s  %s\n", "arch", "name", "slice-use", "mgefs",
                "world-use", "mgefs", "world by source (SPEL ALCH SCRL ENCH INGR other)");
    for (const auto& [a, row] : sorted) {
        std::printf("%4u  %-20s %9zu %6zu  %9zu %6zu  %zu %zu %zu %zu %zu %zu\n", a,
                    archetype_name(a).c_str(), row.slice_uses, row.slice_mgefs.size(),
                    row.world_uses, row.world_mgefs.size(), row.world_by_source[spel],
                    row.world_by_source[alch], row.world_by_source[scrl], row.world_by_source[ench],
                    row.world_by_source[ingr], row.world_by_source[other]);
    }
    // The MGEFs of the slice by archetype, for reading the table.
    std::printf("\nslice MGEFs by archetype (editor ids)\n");
    for (const auto& [a, row] : sorted) {
        if (row.slice_mgefs.empty()) {
            continue;
        }
        std::printf("%u %s:", a, archetype_name(a).c_str());
        std::size_t shown = 0;
        for (const auto id : row.slice_mgefs) {
            if (const auto m = world.magic_effect(id)) {
                std::printf(" %s", m->editor_id.c_str());
            }
            if (++shown == 12) {
                std::printf(" ... (%zu)", row.slice_mgefs.size());
                break;
            }
        }
        std::printf("\n");
    }
}

} // namespace bethconv::cli
