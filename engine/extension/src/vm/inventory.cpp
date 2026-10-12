// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/inventory.hpp"

#include <algorithm>

namespace skydot::vm {

void Inventory::add(std::uint32_t owner, std::uint32_t item, std::int64_t count) {
    if (count > 0) {
        owners_[owner][item] += count;
    }
}

std::int64_t Inventory::remove(std::uint32_t owner, std::uint32_t item, std::int64_t count) {
    const auto o = owners_.find(owner);
    if (o == owners_.end() || count <= 0) {
        return 0;
    }
    const auto i = o->second.find(item);
    if (i == o->second.end()) {
        return 0;
    }
    const std::int64_t taken = std::min(count, i->second);
    i->second -= taken;
    if (i->second <= 0) {
        o->second.erase(i);
        if (o->second.empty()) {
            owners_.erase(o);
        }
    }
    return taken;
}

std::int64_t Inventory::count(std::uint32_t owner, std::uint32_t item) const {
    const auto o = owners_.find(owner);
    if (o == owners_.end()) {
        return 0;
    }
    const auto i = o->second.find(item);
    return i == o->second.end() ? 0 : i->second;
}

const Inventory::Items& Inventory::items(std::uint32_t owner) const {
    static const Items none;
    const auto o = owners_.find(owner);
    return o == owners_.end() ? none : o->second;
}

} // namespace skydot::vm
