// SPDX-License-Identifier: GPL-3.0-or-later
#include "data/leveled.hpp"

#include "data/fb_search.hpp"

#include "skydot_formats/flags.hpp"

#include <algorithm>

namespace skydot::leveled {

namespace wfb = bethconv::pack::wfb;

namespace {

constexpr std::uint64_t k_golden = 0x9E3779B97F4A7C15ULL;

std::uint64_t mix64(std::uint64_t z) noexcept {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

const wfb::LeveledList* list_of(const wfb::World& w, std::uint32_t id) {
    return lookup(w.leveled_lists(), id);
}

/// Adds `entry` to `out`, merging with an earlier one of the same form.
void add(std::vector<Entry>& out, Entry entry) {
    if (entry.count <= 0) {
        return;
    }
    for (auto& e : out) {
        if (e.form == entry.form) {
            e.count += entry.count;
            return;
        }
    }
    out.push_back(entry);
}

void evaluate(const wfb::World& w, const wfb::LeveledList& list, std::uint16_t level, Rng& rng,
              const GlobalValue& global, int depth, std::vector<Entry>& out) {
    // One roll per evaluation, also when the list turns out to have nothing.
    double chance = list.chance_none();
    if (list.chance_global() != 0 && global) {
        if (const auto value = global(list.chance_global())) {
            chance = *value;
        }
    }
    if (chance > 0.0 && static_cast<double>(rng.below(100)) < chance) {
        return;
    }
    const auto* entries = list.entries();
    if (entries == nullptr || entries->size() == 0) {
        return;
    }
    // The entries that are low enough, else the lowest level there is.
    std::vector<const wfb::LeveledEntry*> eligible;
    for (const auto* e : *entries) {
        if (e->level() <= level) {
            eligible.push_back(e);
        }
    }
    if (eligible.empty()) {
        std::uint16_t lowest = 0xFFFF;
        for (const auto* e : *entries) {
            lowest = std::min(lowest, e->level());
        }
        for (const auto* e : *entries) {
            if (e->level() == lowest) {
                eligible.push_back(e);
            }
        }
    }
    if (!formats::has_flag(list.flags(), wfb::LeveledListFlags::from_all_levels)) {
        std::uint16_t top = 0;
        for (const auto* e : eligible) {
            top = std::max(top, e->level());
        }
        std::erase_if(eligible, [top](const wfb::LeveledEntry* e) { return e->level() != top; });
    }
    if (!formats::has_flag(list.flags(), wfb::LeveledListFlags::use_all)) {
        const auto* picked = eligible[rng.below(static_cast<std::uint32_t>(eligible.size()))];
        eligible.assign(1, picked);
    }
    const bool each = formats::has_flag(list.flags(), wfb::LeveledListFlags::each_entry);
    for (const auto* e : eligible) {
        const auto* nested = depth + 1 < k_max_depth ? list_of(w, e->form()) : nullptr;
        if (nested == nullptr) {
            add(out, {e->form(), e->count()});
        } else if (each) {
            for (std::uint16_t i = 0; i < e->count(); ++i) {
                evaluate(w, *nested, level, rng, global, depth + 1, out);
            }
        } else {
            std::vector<Entry> inner;
            evaluate(w, *nested, level, rng, global, depth + 1, inner);
            for (auto& item : inner) {
                item.count *= e->count();
                add(out, item);
            }
        }
    }
}

} // namespace

std::uint64_t Rng::next() noexcept {
    state_ += k_golden;
    return mix64(state_);
}

std::uint32_t Rng::below(std::uint32_t n) noexcept {
    if (n == 0) {
        return 0;
    }
    return static_cast<std::uint32_t>((next() >> 32) * n >> 32);
}

std::uint64_t seed_for(std::uint32_t owner, std::uint64_t purpose) noexcept {
    return mix64((static_cast<std::uint64_t>(owner) * k_golden) ^ (k_leveled_salt + purpose));
}

std::uint16_t player_level() noexcept {
    return 1;
}

bool is_list(const wfb::World& world, std::uint32_t form) {
    return list_of(world, form) != nullptr;
}

std::vector<Entry> resolve(const wfb::World& world, std::uint32_t list, std::uint16_t level, Rng& rng,
                           const GlobalValue& global) {
    std::vector<Entry> out;
    if (const auto* l = list_of(world, list)) {
        evaluate(world, *l, level, rng, global, 0, out);
    }
    return out;
}

std::uint32_t pick_one(const wfb::World& world, std::uint32_t form, std::uint16_t level, Rng& rng,
                       const GlobalValue& global) {
    if (!is_list(world, form)) {
        return form;
    }
    const auto items = resolve(world, form, level, rng, global);
    return items.empty() ? 0 : items.front().form;
}

} // namespace skydot::leveled
