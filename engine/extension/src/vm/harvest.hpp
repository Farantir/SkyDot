// SPDX-License-Identifier: GPL-3.0-or-later
//
// Harvesting plants and trees (FLOR and TREE bases with PFIG): the rules and
// the state that a save keeps. Activating one that is not harvested rolls the
// chance of the season (PFPC, one byte each), gives its ingredient on success,
// and marks the reference harvested either way; a harvested reference gives
// nothing until it respawns. Section HRVS of a save (optional: an older save
// has everything unharvested). Nothing here touches Godot.
//
//   HRVS  u32 references, each u32 ref, f64 game day harvested, u32 times harvested
#pragma once

#include "save/bytes.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace skydot::vm {

/// Game days after which a harvested reference can be harvested again.
/// PLACEHOLDER until cell reset exists (W11), which respawns what the game
/// respawns.
inline constexpr double k_harvest_respawn_days = 10.0;

enum class Season : std::uint8_t { spring, summer, autumn, winter };

/// Spring is months 2-4, summer 5-7, autumn 8-10, winter 11, 0 and 1
/// (`month` 0-based, 0 is Morning Star).
[[nodiscard]] Season season_of_month(int month) noexcept;
/// The chance in percent PFPC gives for `season`: byte 0 spring ... 3 winter.
[[nodiscard]] int harvest_chance(std::uint32_t seasonal_yield, Season season) noexcept;
/// Whether the roll succeeds: the same for one reference and harvest count.
[[nodiscard]] bool harvest_succeeds(int chance_percent, std::uint32_t ref, std::uint32_t times_harvested) noexcept;

class HarvestStates {
public:
    static constexpr std::uint32_t k_section_version = 1;

    /// Whether `ref` is harvested at game day `now` (respawned ones are not).
    [[nodiscard]] bool harvested(std::uint32_t ref, double now) const;
    /// How many times `ref` was harvested (counting respawned ones).
    [[nodiscard]] std::uint32_t times(std::uint32_t ref) const;
    void mark(std::uint32_t ref, double now);
    void erase(std::uint32_t ref) { states_.erase(ref); }
    void clear() { states_.clear(); }
    void adopt(HarvestStates&& other) { *this = std::move(other); }
    [[nodiscard]] bool empty() const { return states_.empty(); }

    [[nodiscard]] std::vector<std::uint8_t> save() const;
    [[nodiscard]] static bool load(save::ByteReader r, HarvestStates& out);

private:
    struct State {
        double day = 0.0;
        std::uint32_t times = 0;
    };
    std::map<std::uint32_t, State> states_;
};

} // namespace skydot::vm
