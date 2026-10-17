// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a container or an actor holds before anything touched it, from
// world.fb alone: CNTO entries, with leveled lists drawn as leveled.hpp
// draws them, and for an actor the default outfit besides. Seeded by the
// owner reference, so the VM and the actor builder get the same items.
#pragma once

#include "data/leveled.hpp"

#include "world_generated.h"

#include <cstdint>
#include <vector>

namespace skydot {

struct StartItem {
    std::uint32_t form{};
    std::int64_t count{};
};

struct StartInventory {
    /// Everything carried, the worn outfit included, merged by form.
    std::vector<StartItem> items;
    /// The default outfit's pieces in order (they are in `items` too).
    std::vector<std::uint32_t> worn;
};

/// The CNTO of container base `base` for reference `owner`; an entry that is
/// a leveled list is drawn `count` times. Empty if `base` holds nothing.
[[nodiscard]] std::vector<StartItem> container_contents(const bethconv::pack::wfb::World& world,
                                                        std::uint32_t base, std::uint32_t owner,
                                                        std::uint16_t level,
                                                        const leveled::GlobalValue& global = {});

/// What actor `owner` of NPC_ `npc` starts with: its CNTO (through the
/// template if it takes its inventory from one) and its default outfit, drawn
/// at the actor's level. `npc` is the NPC_ itself: a leveled NPC list is
/// picked first (see `pick_actor` in data/actors.hpp).
[[nodiscard]] StartInventory actor_contents(const bethconv::pack::wfb::World& world, std::uint32_t npc,
                                            std::uint32_t owner, const leveled::GlobalValue& global = {});

} // namespace skydot
