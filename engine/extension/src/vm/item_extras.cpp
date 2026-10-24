// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/item_extras.hpp"

#include <bit>
#include <cmath>

namespace skydot::vm {

bool ItemExtra::same(const ItemExtra& other) const noexcept {
    return std::bit_cast<std::uint32_t>(health) == std::bit_cast<std::uint32_t>(other.health) &&
           enchantment == other.enchantment && name == other.name &&
           std::bit_cast<std::uint32_t>(charge) == std::bit_cast<std::uint32_t>(other.charge) &&
           std::bit_cast<std::uint32_t>(charge_max) == std::bit_cast<std::uint32_t>(other.charge_max) &&
           poison == other.poison && poison_doses == other.poison_doses;
}

bool ItemExtra::plain() const noexcept { return same(ItemExtra{}); }

const char* temper_tier_name(float health) noexcept {
    const char* name = "";
    for (const auto& tier : k_temper_tiers) {
        if (health >= tier.health - 0.01F) {
            name = tier.name;
        }
    }
    return name;
}

std::uint32_t ItemExtras::intern(const ItemExtra& extra) {
    if (extra.plain()) {
        return 0;
    }
    for (std::size_t i = 0; i < table_.size(); ++i) {
        if (table_[i].same(extra)) {
            return static_cast<std::uint32_t>(i + 1);
        }
    }
    table_.push_back(extra);
    return static_cast<std::uint32_t>(table_.size());
}

bool ItemExtras::replace(std::uint32_t id, const ItemExtra& extra) {
    if (id == 0 || id > table_.size() || extra.plain()) {
        return false;
    }
    for (std::size_t i = 0; i < table_.size(); ++i) {
        if (i + 1 != id && table_[i].same(extra)) {
            return false;
        }
    }
    table_[id - 1] = extra;
    return true;
}

const ItemExtra* ItemExtras::find(std::uint32_t id) const noexcept {
    return id == 0 || id > table_.size() ? nullptr : &table_[id - 1];
}

void ItemExtras::write(save::ByteWriter& w) const {
    w.u32(static_cast<std::uint32_t>(table_.size()));
    for (const auto& e : table_) {
        w.f64(static_cast<double>(e.health));
        w.u32(e.enchantment);
        w.text(e.name);
        w.f64(static_cast<double>(e.charge));
        w.f64(static_cast<double>(e.charge_max));
        w.u32(e.poison);
        w.u32(e.poison_doses);
    }
}

bool ItemExtras::read(save::ByteReader& r, ItemExtras& out, std::uint32_t version) {
    out.clear();
    std::uint32_t n = 0;
    if (!r.count(n, 16)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        ItemExtra e;
        double health = 0.0;
        if (!r.f64(health) || !r.u32(e.enchantment) || !r.text(e.name)) {
            return false;
        }
        e.health = static_cast<float>(health);
        if (version >= 2) {
            double charge = 0.0, charge_max = 0.0;
            if (!r.f64(charge) || !r.f64(charge_max) || !r.u32(e.poison) || !r.u32(e.poison_doses) ||
                !std::isfinite(charge) || !std::isfinite(charge_max)) {
                return false;
            }
            e.charge = static_cast<float>(charge);
            e.charge_max = static_cast<float>(charge_max);
        }
        out.table_.push_back(std::move(e));
    }
    return true;
}

} // namespace skydot::vm
