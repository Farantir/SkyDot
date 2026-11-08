// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which idle an actor plays for an action, as the game asks its IDLE tree: the
// action's (AACT) children are one root per behaviour graph; the root whose
// behaviour (DNAM) is the actor's root behaviour file is searched depth first,
// siblings in the order of their previous-sibling chain. A node is entered when
// its own conditions pass; the first entered leaf with an event wins; an entered
// group none of whose descendants wins is left, and the search goes on with its
// next sibling. Subject = the actor, target = the furniture (or other object).
// No Godot in here.
#pragma once

#include "conditions/conditions.hpp"
#include "data/world_data.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace skydot::ai {

struct IdleChoice {
    std::uint32_t idle{}; ///< The IDLE record.
    std::string event;    ///< Its event, to send to the actor's behaviour graph.
};

/// `root_behaviour` is the actor's root behaviour graph as the pack spells it
/// (`Actors\Character\Behaviors\0_Master.hkx`); case and slash style do not matter.
std::optional<IdleChoice> choose_idle(const WorldData& data, std::uint32_t action, const std::string& root_behaviour,
                                      conditions::ConditionWorld& world, const conditions::ConditionContext& context);

} // namespace skydot::ai
