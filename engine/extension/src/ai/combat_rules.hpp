// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rules of combat AI (GAMEPLAY-PLAN.md, 6h), without Godot, the pack or
// the clock: who is hostile to whom, who starts a fight and who joins it,
// when an actor flees, and what the next step of a melee or ranged fight is.
// SkydotAi gathers the facts and acts on the answer.
//
// Sources: UESP Skyrim:AI Packages / Skyrim:Combat for aggression (0
// unaggressive, 1 aggressive, 2 very aggressive, 3 frenzied) and assistance
// (0 helps nobody, 1 allies, 2 friends and allies), the CK's relationship ranks
// (RELA: 0 lover, 1 ally, 2 confidant, 3 friend, 4 acquaintance, 5 rival, 6
// foe, 7 enemy, 8 archnemesis) and FACT XNAM reactions (0 neutral, 1 enemy, 2
// ally, 3 friend).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace skydot::combat_ai {

enum class Reaction : std::uint8_t { enemy, neutral, ally, friend_ };

/// A faction an actor belongs to; a negative rank means not a member.
struct Membership {
    std::uint32_t faction{};
    std::int32_t rank{};
};

/// One FACT XNAM entry: how members of the owning faction see `faction`.
struct FactionRelation {
    std::uint32_t faction{};
    std::int32_t modifier{};
    std::uint32_t reaction{}; ///< 0 neutral, 1 enemy, 2 ally, 3 friend.
};

/// The XNAM entries of a faction (empty for an unknown one).
using RelationsOf = std::function<std::vector<FactionRelation>(std::uint32_t faction)>;

/// How `a` sees `b`: a relationship (RELA rank) decides first (lover, ally,
/// confidant, friend are friendly; foe, enemy, archnemesis hostile; acquaintance
/// and rival say nothing); else a faction both belong to makes them allies;
/// else the faction relations of either side: any ally or friend entry makes
/// them friendly, otherwise any enemy entry hostile; else neutral.
[[nodiscard]] Reaction reaction(const std::vector<Membership>& a, const std::vector<Membership>& b,
                                std::optional<std::uint32_t> relationship_rank, const RelationsOf& relations);

/// What scripts did to an actor's memberships over the NPC_'s (`out`): `overridden` gives the rank a script set
/// for a faction (below 0: removed), `added` lists the factions scripts put it in; ones `out` lacks are appended.
void apply_scripted_memberships(std::vector<Membership>& out,
                                const std::function<std::optional<std::int32_t>(std::uint32_t faction)>& overridden,
                                const std::vector<std::uint32_t>& added);

/// The XNAM entries of a faction with what scripts set (SetEnemy, SetAlly: faction and reaction): a scripted
/// reaction replaces the data's entry for that faction, others stay.
[[nodiscard]] std::vector<FactionRelation> with_scripted_relations(
    std::vector<FactionRelation> data, const std::vector<std::pair<std::uint32_t, std::uint8_t>>& scripted);

/// A relationship rank a script set (Papyrus scale: 4 lover ... 0 acquaintance ... -4 archnemesis) as the RELA
/// rank `reaction` takes (0 lover ... 8 archnemesis).
[[nodiscard]] constexpr std::uint32_t rela_rank_of(std::int32_t papyrus_rank) noexcept {
    return static_cast<std::uint32_t>(4 - (papyrus_rank < -4 ? -4 : papyrus_rank > 4 ? 4 : papyrus_rank));
}

[[nodiscard]] inline bool friendly(Reaction r) noexcept { return r == Reaction::ally || r == Reaction::friend_; }

/// Whether an actor of `aggression` starts a fight with someone it sees and
/// `reaction` to: 0 never, 1 enemies, 2 enemies and neutrals, 3 anyone.
[[nodiscard]] bool starts_combat(std::uint8_t aggression, Reaction reaction) noexcept;

/// Whether an actor of `assistance` joins a fight of someone it `reaction`s to:
/// 0 never, 1 allies, 2 friends and allies.
[[nodiscard]] bool assists(std::uint8_t assistance, Reaction reaction) noexcept;

/// The game settings the flee rule reads, with their values in Skyrim.esm.
struct FleeSettings {
    float cautious = 0.375F; ///< fConfidenceCautious
    float average = 0.15F;   ///< fConfidenceAverage
    float brave = 0.0375F;   ///< fConfidenceBrave
};

/// The health fraction (0 to 1) below which an actor of `confidence` (0
/// cowardly, 1 cautious, 2 average, 3 brave, 4 foolhardy) flees. PLACEHOLDER
/// reading of the GMSTs as that fraction; cowardly flees at once (above 1),
/// foolhardy never (below 0).
[[nodiscard]] float flee_fraction(std::uint8_t confidence, const FleeSettings& settings) noexcept;

/// The combat style values the melee step reads (CSTY).
struct MeleeStyle {
    float offensive_mult = 0.5F;
    float circle_mult = 0.5F;
    float fallback_mult = 0.5F;
    float special_attack_mult = 0.1F; ///< Takes a power attack.
};

enum class MeleeStep : std::uint8_t { approach, attack, power_attack, circle, fallback, hold };

struct MeleeInputs {
    double distance{}; ///< To the target's body, game units.
    double reach{};    ///< The weapon's, game units.
    bool attack_ready{};
    MeleeStyle style;
    double roll{}; ///< A die in [0, 1).
};

/// PLACEHOLDER: how much of the style's circle and fallback multipliers one
/// step spends as probability while waiting for the next attack.
inline constexpr double k_wait_weight = 0.5;

/// Beyond reach: approach. In reach and ready: attack, a power attack when the
/// die is under the style's special attack multiplier. In reach and waiting:
/// fall back (die under fallback x k_wait_weight), else circle (the next band
/// of circle x k_wait_weight), else hold.
[[nodiscard]] MeleeStep melee_step(const MeleeInputs& in) noexcept;

enum class RangedStep : std::uint8_t { keep_distance, shoot, switch_to_melee, approach };

struct RangedInputs {
    double distance{};  ///< Game units.
    bool line_of_sight{};
    bool has_melee{};   ///< It carries a melee weapon to fall back to.
    double min_distance = 512.0;  ///< PLACEHOLDER: it backs off inside this (GAMEPLAY-PLAN.md 6h).
    double max_distance = 2000.0; ///< PLACEHOLDER: it closes in beyond this.
    double melee_distance = 256.0; ///< PLACEHOLDER: it switches to melee inside this.
};

/// Inside melee_distance with a melee weapon: switch to it; inside min_distance:
/// keep distance; beyond max_distance or without line of sight: approach;
/// otherwise shoot.
[[nodiscard]] RangedStep ranged_step(const RangedInputs& in) noexcept;

// ---- magic (step 7a-4; UESP Skyrim:Calm, Fear, Frenzy, Paralysis, Invisibility, Summon Creature) ----

/// The held magic on an actor as the rules read it (SkydotAi fills it from magic::Overrides).
struct MagicState {
    bool calm = false;
    bool frenzy = false;
    bool paralyzed = false;
    bool invisible = false;
    std::uint32_t fear_from = 0;    ///< Flees from this actor; 0 none.
    std::uint32_t commanded_by = 0; ///< A summon or a reanimated corpse of this actor; 0 none.
};

/// What a mind does this tick: paralysed beats fear beats calm; frenzy and command change who it
/// fights, not what it may do.
enum class MagicMode : std::uint8_t { free, frozen, afraid, calmed };
[[nodiscard]] MagicMode magic_mode(const MagicState& state) noexcept;

/// The aggression it acts on: frenzy 3 (anyone), a commanded actor 1 (its master's enemies), else `base`.
[[nodiscard]] std::uint8_t aggression_under(std::uint8_t base, const MagicState& state) noexcept;

/// A frenzied actor sees everyone as an enemy.
[[nodiscard]] Reaction reaction_under(const MagicState& state, Reaction r) noexcept;

/// Whoever commands `ref` (a summon, a reanimated corpse), 0 if none.
using CommanderOf = std::function<std::uint32_t(std::uint32_t ref)>;
using ReactionOf = std::function<Reaction(std::uint32_t a, std::uint32_t b)>;

/// How `a` sees `b` when either may be commanded: a commanded actor and its commander are allies, and so
/// are two actors of one commander; towards anyone else a commanded actor takes its commander's
/// reaction (and others see it as they see its commander).
[[nodiscard]] Reaction commanded_reaction(std::uint32_t a, std::uint32_t b, const CommanderOf& commander_of,
                                          const ReactionOf& base);

/// After a frenzy ends: whether it goes on fighting `reaction` at `aggression` (else it leaves combat).
[[nodiscard]] inline bool fights_after_frenzy(std::uint8_t aggression, Reaction r) noexcept {
    return starts_combat(aggression, r);
}

enum class FollowStep : std::uint8_t { stay, walk, run };

/// PLACEHOLDERS: a commanded actor walks to within 250 game units of its commander and runs from farther than 600.
inline constexpr double k_follow_near = 250.0;
inline constexpr double k_follow_far = 600.0;
[[nodiscard]] FollowStep follow_step(double distance) noexcept;

/// The stagger clip among a behaviour project's clip names (lower case, no extension): those with
/// "stagger" (not "bash"), else "recoil"; a large one for `large` (name with large, heavy or back),
/// a small one otherwise (small, light), else the shortest, then alphabetically. Empty if none.
/// PLACEHOLDER pick, as the real game chooses by direction and size.
[[nodiscard]] std::string stagger_clip(const std::vector<std::string>& names, bool large);

/// Stagger is large from this magnitude on (PLACEHOLDER).
inline constexpr double k_large_stagger = 0.5;

} // namespace skydot::combat_ai
