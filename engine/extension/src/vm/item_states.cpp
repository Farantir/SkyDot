// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/item_states.hpp"

namespace skydot::vm {

std::uint32_t ItemStates::pick_of(std::uint32_t ref) const {
    const auto it = picks_.find(ref);
    return it == picks_.end() ? 0 : it->second;
}

const Equipment* ItemStates::find(std::uint32_t owner) const {
    const auto it = equipment_.find(owner);
    return it == equipment_.end() ? nullptr : &it->second;
}

void ItemStates::erase(std::uint32_t owner) {
    resolved_.erase(owner);
    picks_.erase(owner);
    equipment_.erase(owner);
    dropped_.erase(owner);
}

void ItemStates::clear() {
    resolved_.clear();
    picks_.clear();
    equipment_.clear();
    dropped_.clear();
}

std::vector<std::uint8_t> ItemStates::save() const {
    save::ByteWriter w;
    w.u32(static_cast<std::uint32_t>(resolved_.size()));
    for (const auto owner : resolved_) {
        w.u32(owner);
    }
    w.u32(static_cast<std::uint32_t>(picks_.size()));
    for (const auto& [ref, npc] : picks_) {
        w.u32(ref);
        w.u32(npc);
    }
    std::uint32_t held = 0;
    for (const auto& [owner, equipment] : equipment_) {
        held += equipment.empty() ? 0U : 1U;
    }
    w.u32(held);
    for (const auto& [owner, equipment] : equipment_) {
        if (equipment.empty()) {
            continue;
        }
        w.u32(owner);
        w.u32(equipment.right);
        w.u32(equipment.left);
        w.u32(equipment.ammo);
        w.u32(static_cast<std::uint32_t>(equipment.worn.size()));
        for (const auto form : equipment.worn) {
            w.u32(form);
        }
    }
    w.u32(static_cast<std::uint32_t>(dropped_.size()));
    for (const auto& [ref, count] : dropped_) {
        w.u32(ref);
        w.u64(static_cast<std::uint64_t>(count));
    }
    return w.out;
}

bool ItemStates::load(save::ByteReader r, ItemStates& out, std::uint32_t version) {
    out = ItemStates{};
    std::uint32_t n = 0;
    if (!r.count(n, 4)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t owner = 0;
        if (!r.u32(owner)) {
            return false;
        }
        out.resolved_.insert(owner);
    }
    if (!r.count(n, 8)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t ref = 0, npc = 0;
        if (!r.u32(ref) || !r.u32(npc)) {
            return false;
        }
        out.picks_[ref] = npc;
    }
    if (!r.count(n, 20)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t owner = 0, worn = 0;
        Equipment equipment;
        if (!r.u32(owner) || !r.u32(equipment.right) || !r.u32(equipment.left) || !r.u32(equipment.ammo) ||
            !r.count(worn, 4)) {
            return false;
        }
        for (std::uint32_t k = 0; k < worn; ++k) {
            std::uint32_t form = 0;
            if (!r.u32(form)) {
                return false;
            }
            equipment.worn.insert(form);
        }
        out.equipment_[owner] = std::move(equipment);
    }
    if (version >= 2) {
        if (!r.count(n, 12)) {
            return false;
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t ref = 0;
            std::uint64_t count = 0;
            if (!r.u32(ref) || !r.u64(count)) {
                return false;
            }
            out.dropped_[ref] = static_cast<std::int64_t>(count);
        }
    }
    return r.at_end();
}

} // namespace skydot::vm
