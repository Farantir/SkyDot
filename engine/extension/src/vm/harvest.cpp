// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/harvest.hpp"

#include "data/leveled.hpp"

namespace skydot::vm {

Season season_of_month(int month) noexcept {
    if (month >= 2 && month <= 4) {
        return Season::spring;
    }
    if (month >= 5 && month <= 7) {
        return Season::summer;
    }
    if (month >= 8 && month <= 10) {
        return Season::autumn;
    }
    return Season::winter;
}

int harvest_chance(std::uint32_t seasonal_yield, Season season) noexcept {
    return static_cast<int>((seasonal_yield >> (8 * static_cast<unsigned>(season))) & 0xFFU);
}

bool harvest_succeeds(int chance_percent, std::uint32_t ref, std::uint32_t times_harvested) noexcept {
    if (chance_percent <= 0) {
        return false;
    }
    if (chance_percent >= 100) {
        return true;
    }
    leveled::Rng rng(leveled::seed_for(ref, 0x48525654ULL + times_harvested)); // "HRVT" + count
    return rng.below(100) < static_cast<std::uint32_t>(chance_percent);
}

bool HarvestStates::harvested(std::uint32_t ref, double now) const {
    const auto it = states_.find(ref);
    return it != states_.end() && it->second.times > 0 && now - it->second.day < k_harvest_respawn_days;
}

std::uint32_t HarvestStates::times(std::uint32_t ref) const {
    const auto it = states_.find(ref);
    return it == states_.end() ? 0 : it->second.times;
}

void HarvestStates::mark(std::uint32_t ref, double now) {
    auto& state = states_[ref];
    state.day = now;
    ++state.times;
}

std::vector<std::uint8_t> HarvestStates::save() const {
    save::ByteWriter w;
    w.u32(static_cast<std::uint32_t>(states_.size()));
    for (const auto& [ref, state] : states_) {
        w.u32(ref);
        w.f64(state.day);
        w.u32(state.times);
    }
    return w.out;
}

bool HarvestStates::load(save::ByteReader r, HarvestStates& out) {
    out.clear();
    std::uint32_t n = 0;
    if (!r.count(n, 16)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t ref = 0;
        State state;
        if (!r.u32(ref) || !r.f64(state.day) || !r.u32(state.times)) {
            return false;
        }
        out.states_[ref] = state;
    }
    return r.at_end();
}

} // namespace skydot::vm
