// SPDX-License-Identifier: GPL-3.0-or-later
//
// A WorldAccess over a world of a few references, for natives tests.
#pragma once

#include "vm/world_access.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace skydot::testing {

/// 1 -> 2 -> 3 (end); 5 -> 6 -> 5 (a loop).
struct FakeWorld final : skydot::vm::WorldAccess {
    std::map<std::uint32_t, std::uint32_t> links{{1, 2}, {2, 3}, {5, 6}, {6, 5}};
    std::uint32_t base_form(std::uint32_t) const override { return 0; }
    std::uint32_t linked_ref(std::uint32_t ref, std::uint32_t keyword) const override {
        const auto it = links.find(ref);
        return keyword == 0 && it != links.end() ? it->second : 0;
    }
    std::uint32_t parent_cell(std::uint32_t ref) const override { return ref == 1 ? 0x100 : 0; }
    bool cell_interior(std::uint32_t cell) const override { return cell == 0x100; }
    bool cell_attached(std::uint32_t cell) const override { return cell == 0x100; }
    bool has_keyword(std::uint32_t form, std::uint32_t keyword) const override {
        return (form == 1 || form == 0x700) && keyword == 0x500; // reference 1 and its base 0x700
    }
    std::uint32_t find_keyword(const std::string& editor_id) const override {
        return editor_id == "TestKeyword" ? 0x500 : 0;
    }
    bool form_list(std::uint32_t list, std::vector<std::uint32_t>& forms) const override {
        if (list != 0x600) {
            return false;
        }
        forms = {0x10, 0x20};
        return true;
    }
    /// MESG records by form id.
    std::map<std::uint32_t, vm::MessageText> messages;
    bool message(std::uint32_t id, vm::MessageText& out) const override {
        const auto it = messages.find(id);
        if (it == messages.end()) {
            return false;
        }
        out = it->second;
        return true;
    }
    bool actor_base(std::uint32_t, vm::ActorBase&) const override { return false; }
    /// Game.GetFormFromFile: "plugin:id" -> form.
    std::map<std::string, std::uint32_t> file_forms;
    std::uint32_t form_from_file(std::int64_t id, const std::string& plugin) const override {
        const auto it = file_forms.find(plugin + ":" + std::to_string(id));
        return it != file_forms.end() ? it->second : 0;
    }
    /// What the data places references with, where they are now and whether they are disabled.
    std::map<std::uint32_t, vm::RefPlacement> placements;
    std::map<std::uint32_t, vm::Xyz> positions;
    std::map<std::uint32_t, vm::Xyz> rotations;
    std::set<std::uint32_t> disabled;
    vm::RefPlacement ref_placement(std::uint32_t ref) const override {
        const auto it = placements.find(ref);
        return it != placements.end() ? it->second : vm::RefPlacement{};
    }
    vm::Xyz ref_position(std::uint32_t ref) const override {
        const auto it = positions.find(ref);
        return it != positions.end() ? it->second : ref_placement(ref).position;
    }
    vm::Xyz ref_rotation(std::uint32_t ref) const override {
        const auto it = rotations.find(ref);
        return it != rotations.end() ? it->second : ref_placement(ref).rotation;
    }
    bool ref_disabled(std::uint32_t ref) const override { return disabled.contains(ref); }
    /// NPC_ records with the unique flag, and PACK -> owning quest.
    std::set<std::uint32_t> unique_npcs;
    std::map<std::uint32_t, std::uint32_t> package_quests;
    bool npc_unique(std::uint32_t npc) const override { return unique_npcs.contains(npc); }
    std::uint32_t package_owner_quest(std::uint32_t package) const override {
        const auto it = package_quests.find(package);
        return it != package_quests.end() ? it->second : 0;
    }
    /// References with their 3D loaded.
    std::set<std::uint32_t> loaded_3d;
    bool ref_3d_loaded(std::uint32_t ref) const override { return loaded_3d.contains(ref); }
    std::int32_t player_level() const override { return 7; }
    double game_days() const override { return 3.25; }
    double time_scale() const override { return 20.0; }
};

} // namespace skydot::testing
