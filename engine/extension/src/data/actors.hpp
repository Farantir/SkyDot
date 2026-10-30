// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a placed actor (ACHR) is built from, decided from world.fb alone:
// its NPC_ with templates resolved, race and sex, the models it wears and
// the skeleton and idle that animate it. `SkydotWorld` builds it.
//
// Rules (from the game's data; see docs/actors.md):
// - Template flags pick what comes from TPLT: traits give race, sex,
//   skin, height, weight and face; inventory the outfit. A leveled
//   list (LVLN, LVLI) is resolved by data/leveled.hpp, the same every time
//   for a reference.
// - Worn: the outfit's armor. Its slots hide the skin's addons that overlap
//   them; the rest of the skin (body, hands, feet) stays.
// - An addon fits a race named by RNAM or its additional races (or the
//   race's armor race). Male and female models fall back to each other.
//   A weight slider means `_0`/`_1` variants; weight picks one.
// - Head: the FaceGen NIF the game precomputes per NPC, when the pack has it.
#pragma once

#include "world_generated.h"

#include "vm/actor_values.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace skydot {

/// What a flying race (RACE flag 0x80, Flies) gives the flight mover: the DATA flight fields and
/// the speeds of its flying movement type (FLMV).
struct FlightPlan {
    bool flies{};
    float max_turn_rate{};        ///< FLMV rotate, radians per second.
    float angular_acceleration{}; ///< RACE.
    float angular_tolerance{};    ///< RACE, degrees.
    float aim_tolerance{};        ///< RACE, degrees.
    float flight_radius{};        ///< RACE, game units.
    float cruise_speed{};         ///< FLMV forward walk, units per second.
    float run_speed{};            ///< FLMV forward run.
    float injured_health{};       ///< RACE: health fraction below which it is injured.
};

struct ActorPlan {
    std::uint32_t npc{};    ///< The NPC_ that supplied traits.
    std::uint32_t race{};
    bool female{};
    /// Radians per second the race turns in place (its walk MOVT's rotate_in_place), 0 without a MOVT.
    float turn_rate{};
    /// Units per second the race walks and runs forward (its walk and run MOVTs times the NPC's
    /// SpeedMult / 100), 0 without a MOVT: the speeds the game moves it at.
    float walk_speed{};
    float run_speed{};
    FlightPlan flight;      ///< Set for a race that flies.
    float scale{1.0F};      ///< NPC height times the race's for its sex.
    /// The traits NPC's QNAM, which body skin (SkinTint) is multiplied by.
    float skin_tone[3]{1.0F, 1.0F, 1.0F};
    std::string skeleton;   ///< Animation skeleton (`.hkx`).
    std::string idle;       ///< An idle clip (`.hkx`), or empty.
    std::string behaviour;  ///< The race's behaviour project for the sex (`.hkx`).
    std::vector<std::string> parts; ///< Models, all skinned to the skeleton.
    /// The models of `parts` that came from the skin (the NPC's WNAM or the race's), where no worn item covers it:
    /// the meshes a skin fade-out takes away, not the worn armor under them (a dragon's underskin).
    std::vector<std::string> skin_parts;
    /// The first-person models of the same armors and skin (ARMA MOD4/MOD5), skinned to
    /// the first-person skeleton; slots the worn items cover are left out as for `parts`.
    std::vector<std::string> parts_1st;
    std::string missing;    ///< Why nothing can be built, or empty.
    /// Worn items cover slot 31 (hair): the FaceGen head's hair is hidden.
    bool hide_hair{};
};

/// The actor placed as `ref` whose base is `id`: `id` itself if it is an NPC_,
/// what a leveled NPC list picks (the same every time for the reference,
/// leveled.hpp) if it is a list; 0 if the list picks nothing.
std::uint32_t pick_actor_form(const bethconv::pack::wfb::World& world, std::uint32_t id, std::uint32_t ref);

/// The level an actor is built at, for the leveled lists of its outfit and
/// inventory: its own level, or for one that levels with the player (the level
/// word a multiplier in thousandths) the player's times it, within its
/// calculated minimum and maximum.
std::uint16_t actor_level(const bethconv::pack::wfb::Npc& npc);

/// The NPC_ that supplies what `flag` covers for `npc` placed as `ref`:
/// TPLT followed while the flag is set, leveled lists picked as plan_actor
/// picks them. Null if `npc` is no NPC_.
const bethconv::pack::wfb::Npc* resolve_npc(const bethconv::pack::wfb::World& world,
                                            std::uint32_t npc,
                                            bethconv::pack::wfb::NpcTemplateFlags flag,
                                            std::uint32_t ref);

/// The values `npc` (an NPC_, or an LVLN picked as for plan_actor) placed as
/// `ref` starts with. Stats (health, magicka, stamina, skills, speed) come
/// through a template with use_stats, AIDT through use_ai_data, the race
/// through use_traits. False if `npc` is no NPC_ or its race is unknown.
bool actor_base_values(const bethconv::pack::wfb::World& world, std::uint32_t npc, std::uint32_t ref,
                       skydot::vm::ActorBase& out);

/// What actor `ref` wears when the game knows it (the VM's equipment): the
/// worn ARMO forms into `armors` and true; false to build from the outfit.
using WornProvider = std::function<bool(std::uint32_t ref, std::vector<std::uint32_t>& armors)>;

/// `exists(vpath)` says whether the pack has an asset. Leveled lists are drawn
/// as leveled.hpp does, with the seeds the VM uses, so the actor is the one
/// the VM knows. `worn`, if given, says what the actor wears.
ActorPlan plan_actor(const bethconv::pack::wfb::World& world, std::uint32_t npc, std::uint32_t ref,
                     const std::function<bool(const std::string&)>& exists, const WornProvider& worn = {});

} // namespace skydot
