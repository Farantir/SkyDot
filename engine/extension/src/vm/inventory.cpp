// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/inventory.hpp"

#include <algorithm>

namespace skydot::vm {

void Inventory::add(std::uint32_t owner, std::uint32_t item, std::int64_t count, std::uint32_t extra) {
    if (count <= 0) {
        return;
    }
    owners_[owner][item] += count;
    if (extra != 0) {
        splits_[owner][item][extra] += count;
    }
}

std::vector<Inventory::Taken> Inventory::remove_split(std::uint32_t owner, std::uint32_t item, std::int64_t count) {
    std::vector<Taken> out;
    const auto o = owners_.find(owner);
    if (o == owners_.end() || count <= 0) {
        return out;
    }
    const auto i = o->second.find(item);
    if (i == o->second.end()) {
        return out;
    }
    const std::int64_t total = i->second;
    const std::int64_t taken = std::min(count, total);
    // Plain units first, then the extras in id order.
    const auto split_of = splits_.find(owner);
    Split* split = nullptr;
    if (split_of != splits_.end()) {
        if (const auto s = split_of->second.find(item); s != split_of->second.end()) {
            split = &s->second;
        }
    }
    std::int64_t in_extras = 0;
    if (split != nullptr) {
        for (const auto& [extra, n] : *split) {
            in_extras += n;
        }
    }
    std::int64_t left = taken;
    const std::int64_t plain = std::min(left, total - in_extras);
    if (plain > 0) {
        out.push_back({0, plain});
        left -= plain;
    }
    if (split != nullptr) {
        for (auto it = split->begin(); it != split->end() && left > 0;) {
            const std::int64_t n = std::min(left, it->second);
            out.push_back({it->first, n});
            left -= n;
            it->second -= n;
            it = it->second <= 0 ? split->erase(it) : std::next(it);
        }
    }
    i->second -= taken;
    if (i->second <= 0) {
        o->second.erase(i);
        if (o->second.empty()) {
            owners_.erase(o);
        }
        if (split_of != splits_.end()) {
            split_of->second.erase(item);
        }
    }
    if (split != nullptr && split_of != splits_.end()) {
        if (const auto s = split_of->second.find(item); s != split_of->second.end() && s->second.empty()) {
            split_of->second.erase(s);
        }
        if (split_of->second.empty()) {
            splits_.erase(split_of);
        }
    }
    return out;
}

std::int64_t Inventory::remove(std::uint32_t owner, std::uint32_t item, std::int64_t count) {
    std::int64_t taken = 0;
    for (const auto& part : remove_split(owner, item, count)) {
        taken += part.count;
    }
    return taken;
}

std::int64_t Inventory::remove_extra(std::uint32_t owner, std::uint32_t item, std::uint32_t extra,
                                     std::int64_t count) {
    const std::int64_t taken = std::min(count, count_extra(owner, item, extra));
    if (taken <= 0) {
        return 0;
    }
    if (extra != 0) {
        auto& split = splits_[owner][item];
        split[extra] -= taken;
        if (split[extra] <= 0) {
            split.erase(extra);
        }
        if (split.empty()) {
            splits_[owner].erase(item);
            if (splits_[owner].empty()) {
                splits_.erase(owner);
            }
        }
    }
    auto& total = owners_[owner][item];
    total -= taken;
    if (total <= 0) {
        owners_[owner].erase(item);
        if (owners_[owner].empty()) {
            owners_.erase(owner);
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

std::int64_t Inventory::units_with_extra(std::uint32_t extra) const {
    std::int64_t n = 0;
    for (const auto& [owner, forms] : splits_) {
        for (const auto& [form, split] : forms) {
            const auto it = split.find(extra);
            n += it == split.end() ? 0 : it->second;
        }
    }
    return n;
}

std::int64_t Inventory::count_extra(std::uint32_t owner, std::uint32_t item, std::uint32_t extra) const {
    const auto& split = extras_of(owner, item);
    if (extra != 0) {
        const auto it = split.find(extra);
        return it == split.end() ? 0 : it->second;
    }
    std::int64_t plain = count(owner, item);
    for (const auto& [id, n] : split) {
        plain -= n;
    }
    return plain;
}

const Inventory::Split& Inventory::extras_of(std::uint32_t owner, std::uint32_t item) const {
    static const Split none;
    const auto o = splits_.find(owner);
    if (o == splits_.end()) {
        return none;
    }
    const auto i = o->second.find(item);
    return i == o->second.end() ? none : i->second;
}

const Inventory::Items& Inventory::items(std::uint32_t owner) const {
    static const Items none;
    const auto o = owners_.find(owner);
    return o == owners_.end() ? none : o->second;
}

void Inventory::set_splits(Splits splits) {
    splits_.clear();
    for (auto& [owner, per_item] : splits) {
        for (auto& [item, split] : per_item) {
            std::int64_t room = count(owner, item);
            for (const auto& [extra, n] : split) {
                const std::int64_t kept = std::min(n, room);
                if (extra != 0 && kept > 0) {
                    splits_[owner][item][extra] = kept;
                    room -= kept;
                }
            }
        }
    }
}

} // namespace skydot::vm
