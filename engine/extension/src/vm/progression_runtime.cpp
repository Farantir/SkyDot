// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/progression_runtime.hpp"

#include "data/fb_search.hpp"
#include "data/leveled.hpp"

#include <algorithm>

namespace skydot::vm {
namespace {

constexpr std::size_t k_override_min_bytes = 8;
constexpr std::size_t k_perk_min_bytes = 5;
constexpr int k_template_depth = 8;

} // namespace

std::vector<std::uint8_t> ProgressionState::save() const {
    save::ByteWriter w;
    for (const double v : player.skill_xp) {
        w.f64(v);
    }
    w.f64(player.xp);
    w.u32(player.level);
    w.u32(static_cast<std::uint32_t>(player.perk_points));
    w.u32(player.trainings);
    for (const auto v : player.legendary) {
        w.u32(v);
    }
    w.u32(static_cast<std::uint32_t>(player.books_read.size()));
    for (const auto book : player.books_read) {
        w.u32(book);
    }
    w.u32(static_cast<std::uint32_t>(overrides.size()));
    for (const auto& [ref, perks] : overrides) {
        const auto start = w.out.size();
        w.u32(ref);
        w.u32(static_cast<std::uint32_t>(perks.size()));
        for (const auto& [perk, rank] : perks) {
            w.u32(perk);
            w.u8(rank);
        }
        w.check_record(start, k_override_min_bytes);
    }
    return w.out;
}

bool ProgressionState::load(save::ByteReader r, ProgressionState& out, std::uint32_t version) {
    out = ProgressionState{};
    if (version != k_section_version) {
        return false;
    }
    PlayerProgress& p = out.player;
    for (double& v : p.skill_xp) {
        if (!r.f64(v)) {
            return false;
        }
    }
    std::uint32_t level = 0;
    std::uint32_t points = 0;
    if (!r.f64(p.xp) || !r.u32(level) || !r.u32(points) || !r.u32(p.trainings) || level > 0xFFFF) {
        return false;
    }
    p.level = static_cast<std::uint16_t>(level);
    p.perk_points = static_cast<std::int32_t>(points);
    for (auto& v : p.legendary) {
        std::uint32_t n = 0;
        if (!r.u32(n) || n > 0xFFFF) {
            return false;
        }
        v = static_cast<std::uint16_t>(n);
    }
    std::uint32_t books = 0;
    if (!r.count(books, 4)) {
        return false;
    }
    for (std::uint32_t i = 0; i < books; ++i) {
        std::uint32_t book = 0;
        if (!r.u32(book)) {
            return false;
        }
        p.books_read.insert(book);
    }
    std::uint32_t refs = 0;
    if (!r.count(refs, k_override_min_bytes)) {
        return false;
    }
    for (std::uint32_t i = 0; i < refs; ++i) {
        std::uint32_t ref = 0;
        std::uint32_t count = 0;
        if (!r.u32(ref) || !r.count(count, k_perk_min_bytes)) {
            return false;
        }
        auto& perks = out.overrides[ref];
        for (std::uint32_t j = 0; j < count; ++j) {
            std::uint32_t perk = 0;
            std::uint8_t rank = 0;
            if (!r.u32(perk) || !r.u8(rank)) {
                return false;
            }
            perks[perk] = rank;
        }
    }
    return r.at_end();
}

std::uint16_t ProgressionRuntime::default_level() const {
    const auto* root = host_ != nullptr ? host_->world_root() : nullptr;
    const auto* npc = root != nullptr ? lookup(root->npcs(), k_player_base) : nullptr;
    return npc != nullptr && npc->level() > 0 ? npc->level() : std::uint16_t{1};
}

void ProgressionRuntime::clear() {
    state_ = ProgressionState{};
    state_.player.level = default_level();
    leveled::set_player_level(state_.player.level);
}

void ProgressionRuntime::adopt(std::optional<ProgressionState> state) {
    if (!state) {
        clear();
        return;
    }
    state_ = std::move(*state);
    leveled::set_player_level(state_.player.level);
}

void ProgressionRuntime::set_skill_xp(std::uint32_t skill_index, double xp) {
    if (skill_index < k_progression_skills) {
        state_.player.skill_xp[skill_index] = xp;
    }
}

void ProgressionRuntime::set_level(std::uint16_t level) {
    state_.player.level = std::max<std::uint16_t>(level, 1);
    leveled::set_player_level(state_.player.level);
}

void ProgressionRuntime::set_legendary(std::uint32_t skill_index, std::uint16_t n) {
    if (skill_index < k_progression_skills) {
        state_.player.legendary[skill_index] = n;
    }
}

std::map<std::uint32_t, std::uint8_t> ProgressionRuntime::base_perks(std::uint32_t ref) const {
    std::map<std::uint32_t, std::uint8_t> out;
    const auto* root = host_ != nullptr ? host_->world_root() : nullptr;
    if (root == nullptr) {
        return out;
    }
    std::uint32_t id = host_->base_of(ref);
    for (int depth = 0; depth < k_template_depth && id != 0; ++depth) {
        const auto* npc = lookup(root->npcs(), id);
        if (npc == nullptr) {
            break;
        }
        const bool inherits = npc->template_() != 0 &&
                              (static_cast<std::uint32_t>(npc->template_flags()) & k_template_spell_list) != 0;
        if (!inherits) {
            if (npc->perks() != nullptr) {
                for (const auto* p : *npc->perks()) {
                    auto& rank = out[p->perk()];
                    rank = std::max(rank, p->rank());
                }
            }
            break;
        }
        id = npc->template_();
    }
    return out;
}

std::uint8_t ProgressionRuntime::perk_rank(std::uint32_t ref, std::uint32_t perk) const {
    const auto it = state_.overrides.find(ref);
    if (it != state_.overrides.end()) {
        const auto changed = it->second.find(perk);
        if (changed != it->second.end()) {
            return changed->second;
        }
    }
    const auto base = base_perks(ref);
    const auto found = base.find(perk);
    return found != base.end() ? found->second : std::uint8_t{0};
}

void ProgressionRuntime::add_perk(std::uint32_t ref, std::uint32_t perk, std::uint8_t rank) {
    const bool had = perk_rank(ref, perk) > 0;
    state_.overrides[ref][perk] = rank;
    if (host_ != nullptr && had != (rank > 0)) {
        if (rank > 0) {
            host_->perk_gained(ref, perk);
        } else {
            host_->perk_lost(ref, perk);
        }
    }
}

std::vector<std::pair<std::uint32_t, std::uint8_t>> ProgressionRuntime::perks_of(std::uint32_t ref) const {
    auto merged = base_perks(ref);
    const auto it = state_.overrides.find(ref);
    if (it != state_.overrides.end()) {
        for (const auto& [perk, rank] : it->second) {
            merged[perk] = rank;
        }
    }
    std::vector<std::pair<std::uint32_t, std::uint8_t>> out;
    for (const auto& [perk, rank] : merged) {
        if (rank > 0) {
            out.emplace_back(perk, rank);
        }
    }
    return out;
}

} // namespace skydot::vm
