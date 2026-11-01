// SPDX-License-Identifier: GPL-3.0-or-later
//
// A WorldAccess over a world of a few references, for natives tests.
#pragma once

#include "vm/world_access.hpp"

#include <cstdint>
#include <map>
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
    bool message(std::uint32_t, vm::MessageText&) const override { return false; }
    bool actor_base(std::uint32_t, vm::ActorBase&) const override { return false; }
    std::int32_t player_level() const override { return 7; }
    double game_days() const override { return 3.25; }
    double time_scale() const override { return 20.0; }
};

} // namespace skydot::testing
