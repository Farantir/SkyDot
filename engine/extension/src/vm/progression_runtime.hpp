// SPDX-License-Identifier: GPL-3.0-or-later
//
// The progression domain of the Papyrus runtime: what the player has earned (skill XP, character XP,
// level, perk points, trainings, legendary skills, books read) and which perks an actor has. It holds
// no rules: the XP formulas are GDScript (Progression), and entry points read the perks from here.
// A perk's effective rank on an actor is its NPC_ base PRKR (through the template when the spell list
// flag says so) changed by overrides (rank 0 removes). Section PRGS of a save (optional: an older save
// has the defaults). No Godot: it reaches the engine through ProgressionRuntime::Host.
#pragma once

#include "save/bytes.hpp"
#include "vm/vm.hpp"
#include "world_generated.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace skydot::vm {

inline constexpr std::uint32_t k_progression_skills = 18;

/// What the player has earned.
struct PlayerProgress {
    /// XP toward the next level of each skill, index = actor value - 6.
    std::array<double, k_progression_skills> skill_xp{};
    /// Character XP toward the next level.
    double xp = 0.0;
    std::uint16_t level = 1;
    std::int32_t perk_points = 0;
    /// Training sessions bought at this level.
    std::uint32_t trainings = 0;
    /// How often each skill was made legendary.
    std::array<std::uint16_t, k_progression_skills> legendary{};
    /// BOOK bases read.
    std::set<std::uint32_t> books_read;

    friend bool operator==(const PlayerProgress&, const PlayerProgress&) = default;
};

/// The part of the runtime a save keeps, detached so a load can decode before it swaps.
struct ProgressionState {
    static constexpr std::uint32_t k_section_version = 1;

    PlayerProgress player;
    /// ref -> perk -> rank; rank 0 removes a perk the base has.
    std::map<std::uint32_t, std::map<std::uint32_t, std::uint8_t>> overrides;

    [[nodiscard]] std::vector<std::uint8_t> save() const;
    [[nodiscard]] static bool load(save::ByteReader r, ProgressionState& out, std::uint32_t version);
};

class ProgressionRuntime {
public:
    class Host {
    public:
        virtual ~Host() = default;
        /// The pack's world records; null without a world.
        [[nodiscard]] virtual const bethconv::pack::wfb::World* world_root() const = 0;
        /// The NPC_ behind an actor reference (0x7 for the player); 0 if none.
        [[nodiscard]] virtual std::uint32_t base_of(std::uint32_t ref) const = 0;
        /// `ref` gained `perk` (it had none) or lost it, by add_perk or remove_perk: the perk's ability and
        /// quest effects are the host's to carry out (perks/entry_points.hpp). Not called by clear or adopt.
        virtual void perk_gained(std::uint32_t /*ref*/, std::uint32_t /*perk*/) {}
        virtual void perk_lost(std::uint32_t /*ref*/, std::uint32_t /*perk*/) {}
        /// Game.AdvanceSkill / IncrementSkill(By): the rules are GDScript (Progression); false if nobody applied them.
        virtual bool advance_skill(std::uint32_t /*skill_av*/, double /*amount*/) { return false; }
        virtual bool increment_skill(std::uint32_t /*skill_av*/, std::int32_t /*count*/) { return false; }
        /// The character XP needed to leave `level` (Progression.level_up_threshold: fXPLevelUpBase + fXPLevelUpMult x level).
        [[nodiscard]] virtual double level_threshold(std::int32_t level) const { return 75.0 + 25.0 * level; }
        virtual void log(const std::string& /*text*/) {}
    };

    static constexpr std::uint32_t k_player_ref = 0x14;
    static constexpr std::uint32_t k_player_base = 0x7;
    /// NPC template flag "use spell list" (0x8): the perks come from the template with the spells.
    static constexpr std::uint32_t k_template_spell_list = 0x8;

    void bind(Host& host) { host_ = &host; }
    /// Bind the Papyrus natives: Game.AdvanceSkill, IncrementSkill, IncrementSkillBy, GetPlayerExperience,
    /// SetPlayerExperience, GetExperienceForLevel, AddPerkPoints and the SKSE GetPerkPoints, SetPerkPoints,
    /// ModPerkPoints; Actor.AddPerk, RemovePerk, HasPerk. Needs `bind` first.
    void bind_natives(Vm& vm);

    /// A new game: the defaults, the level from the Player NPC_.
    void clear();
    /// Take a decoded PRGS section, or none (an older save: the defaults).
    void adopt(std::optional<ProgressionState> state);
    [[nodiscard]] std::vector<std::uint8_t> save() const { return state_.save(); }
    [[nodiscard]] static bool load(save::ByteReader r, ProgressionState& out, std::uint32_t version) {
        return ProgressionState::load(r, out, version);
    }
    /// A runtime reference is gone.
    void forget(std::uint32_t ref) { state_.overrides.erase(ref); }
    [[nodiscard]] std::size_t override_count() const { return state_.overrides.size(); }

    // ---- the player
    [[nodiscard]] const PlayerProgress& player() const { return state_.player; }
    /// The level of the Player NPC_ in the pack (1 without one).
    [[nodiscard]] std::uint16_t default_level() const;
    void set_skill_xp(std::uint32_t skill_index, double xp);
    void set_xp(double xp) { state_.player.xp = xp; }
    /// Also what `leveled::player_level()` answers.
    void set_level(std::uint16_t level);
    void set_perk_points(std::int32_t points) { state_.player.perk_points = points; }
    void set_trainings(std::uint32_t n) { state_.player.trainings = n; }
    void set_legendary(std::uint32_t skill_index, std::uint16_t n);
    void mark_book_read(std::uint32_t book) { state_.player.books_read.insert(book); }
    [[nodiscard]] bool is_book_read(std::uint32_t book) const { return state_.player.books_read.contains(book); }

    // ---- perks
    /// The effective rank of `perk` on `ref`; 0 if it has none.
    [[nodiscard]] std::uint8_t perk_rank(std::uint32_t ref, std::uint32_t perk) const;
    [[nodiscard]] bool has_perk(std::uint32_t ref, std::uint32_t perk) const { return perk_rank(ref, perk) > 0; }
    /// Add the perk at rank 1, or set its rank (0 removes it).
    void add_perk(std::uint32_t ref, std::uint32_t perk, std::uint8_t rank = 1);
    void remove_perk(std::uint32_t ref, std::uint32_t perk) { add_perk(ref, perk, 0); }
    /// Every effective perk of `ref` with its rank, sorted by perk id.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint8_t>> perks_of(std::uint32_t ref) const;

    /// The base PRKR of `ref`'s NPC_ (through its template), perk -> rank, before overrides.
    [[nodiscard]] std::map<std::uint32_t, std::uint8_t> base_perks(std::uint32_t ref) const;

private:

    Host* host_ = nullptr;
    ProgressionState state_;
    bool handler_missing_logged_ = false;
};

} // namespace skydot::vm
