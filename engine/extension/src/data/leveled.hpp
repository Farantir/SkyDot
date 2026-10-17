// SPDX-License-Identifier: GPL-3.0-or-later
//
// Leveled lists (LVLI, LVLN, LVSP) resolved the way the game does, from
// world.fb alone and without Godot. Used by the VM (start inventories, AddItem
// of a list) and by the actor builder, with the same generator so both agree.
//
// Rules, per evaluation of a list:
// - Chance none: the list's LVLG global if it has one, else LVLD, as a
//   percentage; one roll, and the evaluation gives nothing if it hits.
// - Eligible entries have `level <= level` (the player's). If none has, the
//   entries of the lowest level are (the game never leaves a list empty only
//   because the player is too low; a list without entries stays empty).
// - Without `from_all_levels`, only the eligible entries at the highest
//   eligible level are candidates.
// - `use_all` takes every candidate, else one is picked at random.
// - A picked entry that is a list is evaluated `count` times if the list it
//   was picked from has `each_entry`, else once with its results' counts
//   multiplied by `count`. Any other form is `count` of itself.
// - Nesting is capped (`k_max_depth`).
// A CNTO entry that is a list with count N is N evaluations (open: to be
// measured in the game).
#pragma once

#include "world_generated.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace skydot::leveled {

/// Mixed with the owner to seed the generator; a different salt per purpose
/// keeps the picks of one reference independent of each other.
inline constexpr std::uint64_t k_leveled_salt = 0x4C564C4953414C54ULL; // "LVLISALT"
/// Purposes added to the salt.
inline constexpr std::uint64_t k_purpose_inventory = 0;
inline constexpr std::uint64_t k_purpose_actor = 1;
/// An actor's default outfit; apart from the inventory so the builder, which
/// knows only the outfit, draws what the VM drew.
inline constexpr std::uint64_t k_purpose_outfit = 2;

/// The nesting of lists followed before giving up.
inline constexpr int k_max_depth = 8;

/// splitmix64: small, seedable, the same on every platform.
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept : state_(seed) {}
    [[nodiscard]] std::uint64_t next() noexcept;
    /// Uniform in [0, n); 0 for n == 0.
    [[nodiscard]] std::uint32_t below(std::uint32_t n) noexcept;

private:
    std::uint64_t state_;
};

/// A seed that is the same for one owner reference every time.
[[nodiscard]] std::uint64_t seed_for(std::uint32_t owner, std::uint64_t purpose = k_purpose_inventory) noexcept;

/// The player's level. One place, until the player levels (P6).
[[nodiscard]] std::uint16_t player_level() noexcept;

/// The value of a GlobalVariable, if it is known.
using GlobalValue = std::function<std::optional<double>(std::uint32_t global)>;

struct Entry {
    std::uint32_t form{};
    std::int64_t count{};
};

/// Whether `form` is a leveled list (LVLI, LVLN or LVSP).
[[nodiscard]] bool is_list(const bethconv::pack::wfb::World& world, std::uint32_t form);

/// The items `list` gives for a player of `level`, merged by form in the
/// order they first came up. Empty if `list` is none or the roll gave nothing.
[[nodiscard]] std::vector<Entry> resolve(const bethconv::pack::wfb::World& world, std::uint32_t list,
                                         std::uint16_t level, Rng& rng, const GlobalValue& global = {});

/// The one form a list picks, followed through nested lists (an LVLN's
/// actor): the first form `resolve` gives; 0 if none. A form that is no list
/// is its own pick.
[[nodiscard]] std::uint32_t pick_one(const bethconv::pack::wfb::World& world, std::uint32_t form,
                                     std::uint16_t level, Rng& rng, const GlobalValue& global = {});

} // namespace skydot::leveled
