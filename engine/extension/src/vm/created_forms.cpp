// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/created_forms.hpp"

#include <bit>
#include <cmath>
#include <utility>

namespace skydot::vm {

bool CreatedEffect::operator==(const CreatedEffect& other) const noexcept {
    return effect == other.effect && std::bit_cast<std::uint32_t>(magnitude) == std::bit_cast<std::uint32_t>(other.magnitude) &&
           duration == other.duration && area == other.area;
}

bool CreatedEnchantment::operator==(const CreatedEnchantment& other) const noexcept {
    return base == other.base && effects == other.effects && std::bit_cast<std::uint64_t>(cost) == std::bit_cast<std::uint64_t>(other.cost) &&
           constant == other.constant;
}

std::uint32_t CreatedForms::create_enchantment(CreatedEnchantment ench) {
    for (const auto& [id, existing] : enchantments_) {
        if (existing == ench) {
            return id;
        }
    }
    ench.id = ids_.allocate();
    if (ench.id == 0) {
        return 0;
    }
    const std::uint32_t id = ench.id;
    enchantments_.emplace(id, std::move(ench));
    return id;
}

const CreatedEnchantment* CreatedForms::find_enchantment(std::uint32_t id) const {
    const auto it = enchantments_.find(id);
    return it == enchantments_.end() ? nullptr : &it->second;
}

std::uint32_t CreatedForms::create(CreatedPotion potion) {
    for (const auto& [id, existing] : potions_) {
        if (existing.poison == potion.poison && existing.effects == potion.effects) {
            return id;
        }
    }
    potion.id = ids_.allocate();
    if (potion.id == 0) {
        return 0;
    }
    const std::uint32_t id = potion.id;
    potions_.emplace(id, std::move(potion));
    return id;
}

const CreatedPotion* CreatedForms::find(std::uint32_t id) const {
    const auto it = potions_.find(id);
    return it == potions_.end() ? nullptr : &it->second;
}

bool CreatedForms::known(std::uint32_t ingredient, std::size_t index) const {
    return index < 8 && (known_mask(ingredient) & (1U << index)) != 0;
}

std::uint8_t CreatedForms::known_mask(std::uint32_t ingredient) const {
    const auto it = known_.find(ingredient);
    return it == known_.end() ? std::uint8_t{0} : it->second;
}

void CreatedForms::learn(std::uint32_t ingredient, std::size_t index) {
    if (index < 4) {
        known_[ingredient] = static_cast<std::uint8_t>(known_mask(ingredient) | (1U << index));
    }
}

void CreatedForms::clear() {
    ids_.clear();
    potions_.clear();
    enchantments_.clear();
    known_.clear();
}

std::vector<std::uint8_t> CreatedForms::save() const {
    save::ByteWriter w;
    w.u32(ids_.next());
    w.u32(static_cast<std::uint32_t>(potions_.size()));
    for (const auto& [id, p] : potions_) {
        w.u32(id);
        w.text(p.name);
        w.u32(static_cast<std::uint32_t>(p.value));
        w.f64(static_cast<double>(p.weight));
        w.u8(p.poison ? 1 : 0);
        w.text(p.model);
        w.u32(static_cast<std::uint32_t>(p.effects.size()));
        for (const auto& e : p.effects) {
            w.u32(e.effect);
            w.f64(static_cast<double>(e.magnitude));
            w.u32(e.duration);
        }
    }
    w.u32(static_cast<std::uint32_t>(known_.size()));
    for (const auto& [form, mask] : known_) {
        w.u32(form);
        w.u8(mask);
    }
    w.u32(static_cast<std::uint32_t>(enchantments_.size()));
    for (const auto& [id, e] : enchantments_) {
        w.u32(id);
        w.text(e.name);
        w.u32(e.base);
        w.f64(e.cost);
        w.u8(e.constant ? 1 : 0);
        w.u32(static_cast<std::uint32_t>(e.effects.size()));
        for (const auto& f : e.effects) {
            w.u32(f.effect);
            w.f64(static_cast<double>(f.magnitude));
            w.u32(f.duration);
            w.u32(f.area);
        }
    }
    return w.out;
}

bool CreatedForms::load(save::ByteReader r, CreatedForms& out, std::uint32_t version) {
    out.clear();
    std::uint32_t next = 0;
    std::uint32_t n = 0;
    if (!r.u32(next) || !out.ids_.restore(next) || !r.count(n, 25)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        CreatedPotion p;
        std::uint32_t value = 0, effects = 0;
        double weight = 0.0;
        std::uint8_t poison = 0;
        if (!r.u32(p.id) || !r.text(p.name) || !r.u32(value) || !r.f64(weight) || !r.u8(poison) ||
            !r.text(p.model) || !r.count(effects, 16) || !id_ranges::created_forms.contains(p.id)) {
            return false;
        }
        p.value = static_cast<std::int32_t>(value);
        p.weight = static_cast<float>(weight);
        p.poison = poison != 0;
        for (std::uint32_t k = 0; k < effects; ++k) {
            CreatedEffect e;
            double magnitude = 0.0;
            if (!r.u32(e.effect) || !r.f64(magnitude) || !r.u32(e.duration)) {
                return false;
            }
            e.magnitude = static_cast<float>(magnitude);
            p.effects.push_back(e);
        }
        out.ids_.note_used(p.id);
        out.potions_.emplace(p.id, std::move(p));
    }
    if (!r.count(n, 5)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t form = 0;
        std::uint8_t mask = 0;
        if (!r.u32(form) || !r.u8(mask)) {
            return false;
        }
        out.known_[form] = static_cast<std::uint8_t>(mask & 0xFU);
    }
    if (version >= 2) {
        if (!r.count(n, 25)) {
            return false;
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            CreatedEnchantment e;
            std::uint8_t constant = 0;
            std::uint32_t effects = 0;
            if (!r.u32(e.id) || !r.text(e.name) || !r.u32(e.base) || !r.f64(e.cost) || !r.u8(constant) ||
                !r.count(effects, 20) || !id_ranges::created_forms.contains(e.id) || !std::isfinite(e.cost)) {
                return false;
            }
            e.constant = constant != 0;
            for (std::uint32_t k = 0; k < effects; ++k) {
                CreatedEffect f;
                double magnitude = 0.0;
                if (!r.u32(f.effect) || !r.f64(magnitude) || !r.u32(f.duration) || !r.u32(f.area)) {
                    return false;
                }
                f.magnitude = static_cast<float>(magnitude);
                e.effects.push_back(f);
            }
            out.ids_.note_used(e.id);
            out.enchantments_.emplace(e.id, std::move(e));
        }
    }
    return r.at_end();
}

} // namespace skydot::vm
