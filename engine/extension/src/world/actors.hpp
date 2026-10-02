// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a placed actor (ACHR) is built from, decided from world.fb alone:
// its NPC_ with templates resolved, race and sex, the models it wears and
// the skeleton and idle that animate it. `SkydotWorld` builds it.
//
// Rules (from the game's data; see docs/actors.md):
// - Template flags pick what comes from TPLT: traits (1) give race, sex,
//   skin, height, weight and face; inventory (0x100) the outfit. A leveled
//   list (LVLN, LVLI) picks one entry, the same every time for a reference.
// - Worn: the outfit's armor. Its slots hide the skin's addons that overlap
//   them; the rest of the skin (body, hands, feet) stays.
// - An addon fits a race named by RNAM or its additional races (or the
//   race's armor race). Male and female models fall back to each other.
//   A weight slider means `_0`/`_1` variants; weight picks one.
// - Head: the FaceGen NIF the game precomputes per NPC, when the pack has it.
#pragma once

#include "world_generated.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace skydot {

struct ActorPlan {
    std::uint32_t npc{};    ///< The NPC_ that supplied traits.
    std::uint32_t race{};
    bool female{};
    float scale{1.0F};      ///< NPC height times the race's for its sex.
    /// The traits NPC's QNAM, which body skin (SkinTint) is multiplied by.
    float skin_tone[3]{1.0F, 1.0F, 1.0F};
    std::string skeleton;   ///< Animation skeleton (`.hkx`).
    std::string idle;       ///< An idle clip (`.hkx`), or empty.
    std::vector<std::string> parts; ///< Models, all skinned to the skeleton.
    std::string missing;    ///< Why nothing can be built, or empty.
    /// Worn items cover slot 31 (hair): the FaceGen head's hair is hidden.
    bool hide_hair{};
};

/// `exists(vpath)` says whether the pack has an asset.
ActorPlan plan_actor(const bethconv::pack::wfb::World& world, std::uint32_t npc, std::uint32_t ref,
                     const std::function<bool(const std::string&)>& exists);

} // namespace skydot
