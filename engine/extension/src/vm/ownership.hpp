// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who owns a reference: what a script set (SetActorOwner, SetFactionOwner) over the data's XOWN, which falls back
// to the cell's. An owner is an NPC_ or a FACT; for a faction, `rank` is the least rank that owns it (-1 every
// member). The Sleep menu refuses a bed owned by anyone but the player (sNoSleepInOwnedBed).
#pragma once

#include "vm/vm.hpp"
#include "vm/world_access.hpp"

namespace skydot::vm {

[[nodiscard]] inline OwnerState owner_of(const Vm& vm, const WorldAccess& world, std::uint32_t ref) {
    if (const OwnerState* set = vm.owners().find(ref)) {
        return *set;
    }
    RefInfo info;
    if (!world.ref_info(ref, info) || info.owner == 0) {
        return {};
    }
    return {.owner = info.owner, .faction = !world.is_npc(info.owner), .rank = info.owner_rank};
}

} // namespace skydot::vm
