// SPDX-License-Identifier: GPL-3.0-or-later
#include "state/item_states.hpp"

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
    note_change();
    resolved_.erase(owner);
    picks_.erase(owner);
    equipment_.erase(owner);
    dropped_.erase(owner);
    dropped_extra_.erase(owner);
}

void ItemStates::clear() {
    note_change();
    resolved_.clear();
    picks_.clear();
    equipment_.clear();
    dropped_.clear();
    dropped_extra_.clear();
    extras_.clear();
    loaded_splits_.clear();
}

namespace {
/// A resolved owner: ref (u32).
constexpr std::size_t k_resolved_owner_min_bytes = 4;
/// A pick: ref (u32), npc (u32).
constexpr std::size_t k_pick_min_bytes = 4 + 4;
/// A worn form: form (u32).
constexpr std::size_t k_worn_form_min_bytes = 4;
/// A worn form's extra: form (u32), extra (u32).
constexpr std::size_t k_worn_extra_min_bytes = 4 + 4;
/// Equipment before version 3: owner, right, left, ammo (u32 each), worn count (u32).
constexpr std::size_t k_equipment_v2_min_bytes = 4 + 4 + 4 + 4 + 4;
/// Equipment: the version 2 fields, right/left/ammo extras (u32 each), worn extra count (u32).
constexpr std::size_t k_equipment_min_bytes = k_equipment_v2_min_bytes + 4 + 4 + 4 + 4;
/// A dropped item before version 3: ref (u32), count (u64).
constexpr std::size_t k_dropped_v2_min_bytes = 4 + 8;
/// A dropped item: ref (u32), count (u64), extra (u32).
constexpr std::size_t k_dropped_min_bytes = k_dropped_v2_min_bytes + 4;
/// A split owner: owner (u32), item count (u32).
constexpr std::size_t k_split_owner_min_bytes = 4 + 4;
/// A split item: form (u32), entry count (u32).
constexpr std::size_t k_split_item_min_bytes = 4 + 4;
/// A split entry: extra (u32), count (u64).
constexpr std::size_t k_split_entry_min_bytes = 4 + 8;
} // namespace

std::vector<std::uint8_t> ItemStates::save(const Inventory& inventory) const {
    save::ByteWriter w;
    w.u32(static_cast<std::uint32_t>(resolved_.size()));
    for (const auto owner : resolved_) {
        const auto start = w.out.size();
        w.u32(owner);
        w.check_record(start, k_resolved_owner_min_bytes);
    }
    w.u32(static_cast<std::uint32_t>(picks_.size()));
    for (const auto& [ref, npc] : picks_) {
        const auto start = w.out.size();
        w.u32(ref);
        w.u32(npc);
        w.check_record(start, k_pick_min_bytes);
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
        const auto start = w.out.size();
        w.u32(owner);
        w.u32(equipment.right);
        w.u32(equipment.left);
        w.u32(equipment.ammo);
        w.u32(static_cast<std::uint32_t>(equipment.worn.size()));
        for (const auto form : equipment.worn) {
            const auto form_start = w.out.size();
            w.u32(form);
            w.check_record(form_start, k_worn_form_min_bytes);
        }
        w.u32(equipment.right_extra);
        w.u32(equipment.left_extra);
        w.u32(equipment.ammo_extra);
        w.u32(static_cast<std::uint32_t>(equipment.worn_extra.size()));
        for (const auto& [form, extra] : equipment.worn_extra) {
            const auto extra_start = w.out.size();
            w.u32(form);
            w.u32(extra);
            w.check_record(extra_start, k_worn_extra_min_bytes);
        }
        w.check_record(start, k_equipment_min_bytes);
    }
    w.u32(static_cast<std::uint32_t>(dropped_.size()));
    for (const auto& [ref, count] : dropped_) {
        const auto start = w.out.size();
        w.u32(ref);
        w.u64(static_cast<std::uint64_t>(count));
        w.u32(dropped_extra(ref));
        w.check_record(start, k_dropped_min_bytes);
    }
    extras_.write(w);
    const auto& splits = inventory.splits();
    w.u32(static_cast<std::uint32_t>(splits.size()));
    for (const auto& [owner, per_item] : splits) {
        const auto start = w.out.size();
        w.u32(owner);
        w.u32(static_cast<std::uint32_t>(per_item.size()));
        for (const auto& [form, split] : per_item) {
            const auto item_start = w.out.size();
            w.u32(form);
            w.u32(static_cast<std::uint32_t>(split.size()));
            for (const auto& [extra, count] : split) {
                const auto entry_start = w.out.size();
                w.u32(extra);
                w.u64(static_cast<std::uint64_t>(count));
                w.check_record(entry_start, k_split_entry_min_bytes);
            }
            w.check_record(item_start, k_split_item_min_bytes);
        }
        w.check_record(start, k_split_owner_min_bytes);
    }
    return w.out;
}

bool ItemStates::load(save::ByteReader r, ItemStates& out, std::uint32_t version) {
    out = ItemStates{};
    std::uint32_t n = 0;
    if (!r.count(n, k_resolved_owner_min_bytes)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t owner = 0;
        if (!r.u32(owner)) {
            return false;
        }
        out.resolved_.insert(owner);
    }
    if (!r.count(n, k_pick_min_bytes)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t ref = 0, npc = 0;
        if (!r.u32(ref) || !r.u32(npc)) {
            return false;
        }
        out.picks_[ref] = npc;
    }
    if (!r.count(n, version >= 3 ? k_equipment_min_bytes : k_equipment_v2_min_bytes)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t owner = 0, worn = 0;
        Equipment equipment;
        if (!r.u32(owner) || !r.u32(equipment.right) || !r.u32(equipment.left) || !r.u32(equipment.ammo) ||
            !r.count(worn, k_worn_form_min_bytes)) {
            return false;
        }
        for (std::uint32_t k = 0; k < worn; ++k) {
            std::uint32_t form = 0;
            if (!r.u32(form)) {
                return false;
            }
            equipment.worn.insert(form);
        }
        if (version >= 3) {
            std::uint32_t extras = 0;
            if (!r.u32(equipment.right_extra) || !r.u32(equipment.left_extra) || !r.u32(equipment.ammo_extra) ||
                !r.count(extras, k_worn_extra_min_bytes)) {
                return false;
            }
            for (std::uint32_t k = 0; k < extras; ++k) {
                std::uint32_t form = 0, extra = 0;
                if (!r.u32(form) || !r.u32(extra)) {
                    return false;
                }
                equipment.worn_extra[form] = extra;
            }
        }
        out.equipment_[owner] = std::move(equipment);
    }
    if (version >= 2) {
        if (!r.count(n, version >= 3 ? k_dropped_min_bytes : k_dropped_v2_min_bytes)) {
            return false;
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t ref = 0, extra = 0;
            std::uint64_t count = 0;
            if (!r.u32(ref) || !r.u64(count) || (version >= 3 && !r.u32(extra))) {
                return false;
            }
            out.set_dropped(ref, static_cast<std::int64_t>(count), extra);
        }
    }
    if (version >= 3) {
        if (!ItemExtras::read(r, out.extras_, version >= 5 ? ItemExtras::k_format_version : (version >= 4 ? 2U : 1U)) || !r.count(n, k_split_owner_min_bytes)) {
            return false;
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            std::uint32_t owner = 0, items = 0;
            if (!r.u32(owner) || !r.count(items, k_split_item_min_bytes)) {
                return false;
            }
            for (std::uint32_t k = 0; k < items; ++k) {
                std::uint32_t form = 0, entries = 0;
                if (!r.u32(form) || !r.count(entries, k_split_entry_min_bytes)) {
                    return false;
                }
                for (std::uint32_t e = 0; e < entries; ++e) {
                    std::uint32_t extra = 0;
                    std::uint64_t count = 0;
                    if (!r.u32(extra) || !r.u64(count)) {
                        return false;
                    }
                    out.loaded_splits_[owner][form][extra] = static_cast<std::int64_t>(count);
                }
            }
        }
    }
    return r.at_end();
}

} // namespace skydot::vm
