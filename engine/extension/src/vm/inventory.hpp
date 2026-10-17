// SPDX-License-Identifier: GPL-3.0-or-later
//
// What references carry: per owner reference, item form -> count. The player's
// is the one that matters; any reference can own items (a container, an actor).
// The count is the total, which scripts and conditions see. Units that carry
// extra data (docs/adr/0007; item_extras.hpp) are split off: per owner and
// form, extra id -> count, summing to at most the total; the rest are plain.
// Nothing here touches Godot.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace skydot::vm {

class Inventory {
public:
    using Items = std::map<std::uint32_t, std::int64_t>;
    /// Extra id -> count of the units that carry it.
    using Split = std::map<std::uint32_t, std::int64_t>;
    using Splits = std::map<std::uint32_t, std::map<std::uint32_t, Split>>;

    /// Units taken: `extra` 0 for the plain ones.
    struct Taken {
        std::uint32_t extra = 0;
        std::int64_t count = 0;
    };

    /// Add `count` (> 0) of `item` to `owner`, as units carrying `extra` (0: plain).
    void add(std::uint32_t owner, std::uint32_t item, std::int64_t count, std::uint32_t extra = 0);
    /// Take up to `count` of `item` from `owner`, plain units first, then the
    /// extras in id order; returns how many were there to take.
    std::int64_t remove(std::uint32_t owner, std::uint32_t item, std::int64_t count);
    /// The same, saying which units were taken.
    std::vector<Taken> remove_split(std::uint32_t owner, std::uint32_t item, std::int64_t count);
    /// Take up to `count` of the units carrying `extra` (0: the plain ones).
    std::int64_t remove_extra(std::uint32_t owner, std::uint32_t item, std::uint32_t extra, std::int64_t count);
    /// The total.
    [[nodiscard]] std::int64_t count(std::uint32_t owner, std::uint32_t item) const;
    /// The units carrying `extra` (0: the plain ones).
    [[nodiscard]] std::int64_t count_extra(std::uint32_t owner, std::uint32_t item, std::uint32_t extra) const;
    /// The units of `item` that carry extras (empty if all are plain).
    [[nodiscard]] const Split& extras_of(std::uint32_t owner, std::uint32_t item) const;
    /// All of `owner`'s items (empty if it has none).
    [[nodiscard]] const Items& items(std::uint32_t owner) const;
    void clear() {
        owners_.clear();
        splits_.clear();
    }
    void erase(std::uint32_t owner) {
        owners_.erase(owner);
        splits_.erase(owner);
    }
    [[nodiscard]] const std::map<std::uint32_t, Items>& owners() const { return owners_; }
    [[nodiscard]] const Splits& splits() const { return splits_; }
    /// Take over saved splits; what exceeds the totals is dropped (the units
    /// stay, as plain ones).
    void set_splits(Splits splits);

private:
    std::map<std::uint32_t, Items> owners_;
    Splits splits_;
};

} // namespace skydot::vm
