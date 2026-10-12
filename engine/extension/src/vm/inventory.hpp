// SPDX-License-Identifier: GPL-3.0-or-later
//
// What references carry: per owner reference, item form -> count. The player's
// is the one that matters; any reference can own items (a container, an actor).
// Nothing here touches Godot.
#pragma once

#include <cstdint>
#include <map>

namespace skydot::vm {

class Inventory {
public:
    using Items = std::map<std::uint32_t, std::int64_t>;

    /// Add `count` (> 0) of `item` to `owner`.
    void add(std::uint32_t owner, std::uint32_t item, std::int64_t count);
    /// Take up to `count` of `item` from `owner`; returns how many were there to take.
    std::int64_t remove(std::uint32_t owner, std::uint32_t item, std::int64_t count);
    [[nodiscard]] std::int64_t count(std::uint32_t owner, std::uint32_t item) const;
    /// All of `owner`'s items (empty if it has none).
    [[nodiscard]] const Items& items(std::uint32_t owner) const;
    void clear() { owners_.clear(); }
    [[nodiscard]] const std::map<std::uint32_t, Items>& owners() const { return owners_; }

private:
    std::map<std::uint32_t, Items> owners_;
};

} // namespace skydot::vm
