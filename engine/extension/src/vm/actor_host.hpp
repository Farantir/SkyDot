// SPDX-License-Identifier: GPL-3.0-or-later
//
// `ActorHost`: what the Actor natives beyond values change or ask that a
// record read (WorldAccess) or a notice (WorldEvents) cannot say: the actor
// states a save carries, the condition view of the world (factions, ranks,
// cells and locations as the AI reads them), and the few things the game
// layer does for an actor. Small enough to stay one interface.
#pragma once

#include "conditions/conditions.hpp"
#include "state/actor_states.hpp"

#include <cstdint>

namespace skydot::vm {

class ActorHost {
public:
    virtual ~ActorHost() = default;

    /// What scripts changed per actor (factions, ghost, relationships, flags, deaths).
    [[nodiscard]] virtual ActorStates& actor_states() = 0;
    /// The world as conditions ask about it: faction ranks, NPC_ of a reference,
    /// cells, worldspaces, locations and their tree.
    [[nodiscard]] virtual conditions::ConditionWorld& conditions() = 0;
    /// Whether `location` or a location below it holds a loaded cell.
    [[nodiscard]] virtual bool location_loaded(std::uint32_t location) = 0;
    /// EvaluatePackage: the AI chooses `ref`'s package again at once.
    virtual void evaluate_package(std::uint32_t ref) = 0;
    /// The player was killed by a script: `killer` (0: nobody) is who hurt him last.
    virtual void set_last_damager(std::uint32_t killer) = 0;
};

} // namespace skydot::vm
