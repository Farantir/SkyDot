// SPDX-License-Identifier: GPL-3.0-or-later
#include "magic/enchanted.hpp"

#include "data/fb_search.hpp"

#include <algorithm>
#include <cmath>

namespace skydot::magic {

double enchantment_cost(const wfb::World& world, const wfb::Enchantment& ench, const MagicSettings& s) {
    if ((ench.flags() & k_ench_manual_cost) != 0 || ench.effects() == nullptr) {
        return static_cast<double>(ench.cost()) * k_placeholder_enchant_cost_scale;
    }
    double sum = 0.0;
    for (const auto* e : *ench.effects()) {
        const auto* m = lookup(world.magic_effects(), e->effect());
        if (m == nullptr) {
            continue;
        }
        sum += effect_cost(static_cast<double>(m->base_cost()), static_cast<double>(e->magnitude()),
                           static_cast<double>(e->duration()), casting::fire_and_forget, static_cast<double>(e->area()), s);
    }
    return sum * k_placeholder_enchant_cost_scale;
}

std::optional<ResolvedEnchantment> resolve_enchantment(const wfb::World& world, const vm::CreatedForms* created,
                                                      std::uint32_t id, const MagicSettings& s) {
    if (id == 0) {
        return std::nullopt;
    }
    ResolvedEnchantment out;
    out.id = id;
    if (const auto* e = lookup(world.enchantments(), id)) {
        out.world = e;
        out.effects = inputs_of(e->effects());
        out.cost = enchantment_cost(world, *e, s);
        out.charge = e->charge();
        out.constant = e->casting_type() == 0;
        out.name = e->name() != nullptr ? e->name()->str() : std::string{};
        return out;
    }
    const auto* made = created != nullptr ? created->find_enchantment(id) : nullptr;
    if (made == nullptr) {
        return std::nullopt;
    }
    out.created = true;
    out.cost = made->cost;
    out.constant = made->constant;
    out.name = made->name;
    for (const auto& f : made->effects) {
        out.effects.push_back({f.effect, f.magnitude, f.area, f.duration, -1});
    }
    return out;
}

float charge_max(float extra_max, std::uint32_t item_amount, std::int32_t enchantment_charge) {
    if (extra_max > 0.0F) {
        return extra_max;
    }
    if (item_amount > 0) {
        return static_cast<float>(item_amount);
    }
    return static_cast<float>(std::max(enchantment_charge, 0));
}

float charge_now(float stored, float max) {
    if (stored < 0.0F) {
        return max; // k_full_charge
    }
    return std::min(stored, max);
}

ChargeSpend spend_charge(float stored, float max, double cost, bool spends) {
    ChargeSpend out;
    out.cost = cost;
    const float now = charge_now(stored, max);
    out.effect = !spends || now > 0.0F;
    out.after = now;
    if (spends && out.effect && cost > 0.0) {
        out.after = static_cast<float>(std::max(0.0, static_cast<double>(now) - cost));
    }
    return out;
}

std::uint32_t enchantment_of(const wfb::World& world, std::uint32_t form, const vm::ItemExtra* extra) {
    if (extra != nullptr && extra->enchantment != 0) {
        return extra->enchantment;
    }
    const auto* base = lookup(world.bases(), form);
    const auto* item = base != nullptr ? base->item() : nullptr;
    return item != nullptr ? item->enchantment() : 0;
}

WeaponCharge weapon_charge(const wfb::World& world, std::uint32_t form, const vm::ItemExtra* extra,
                           const MagicSettings& s, const vm::CreatedForms* created) {
    WeaponCharge out;
    out.enchantment = enchantment_of(world, form, extra);
    const auto ench = resolve_enchantment(world, created, out.enchantment, s);
    if (!ench) {
        out.enchantment = 0;
        return out;
    }
    const auto* base = lookup(world.bases(), form);
    const auto* item = base != nullptr ? base->item() : nullptr;
    out.max = charge_max(extra != nullptr ? extra->charge_max : 0.0F, item != nullptr ? item->enchantment_amount() : 0,
                         ench->charge);
    out.charge = charge_now(extra != nullptr ? extra->charge : vm::k_full_charge, out.max);
    out.cost = ench->cost;
    return out;
}

bool can_poison(const vm::ItemExtra& extra) noexcept { return extra.poison == 0 || extra.poison_doses == 0; }

vm::ItemExtra poisoned(vm::ItemExtra extra, std::uint32_t poison, std::uint32_t doses) {
    extra.poison = poison;
    extra.poison_doses = poison != 0 ? doses : 0;
    return extra;
}

HitMagicResult hit_magic(const wfb::World& world, ActiveEffects& effects, vm::ItemExtras& extras,
                         const HitMagicInput& in, const PoisonInputs& poison_inputs, const MagicSettings& s, const vm::CreatedForms* created) {
    HitMagicResult out;
    out.new_extra = in.extra;
    if (in.weapon == 0) {
        return out;
    }
    vm::ItemExtra extra;
    if (const auto* found = extras.find(in.extra)) {
        extra = *found;
    }
    bool changed = false;

    // The poison on the weapon: delivered once per dose.
    if (extra.poison != 0 && extra.poison_doses > 0) {
        out.poison = extra.poison;
        std::vector<EffectInput> inputs;
        if (in.target_alive && poison_inputs && poison_inputs(extra.poison, inputs) && !inputs.empty()) {
            const auto ids = effects.apply(in.attacker, in.target, {SourceKind::poison, extra.poison, 0}, inputs);
            out.instances.insert(out.instances.end(), ids.begin(), ids.end());
        }
        extra.poison_doses -= 1;
        if (extra.poison_doses == 0) {
            extra.poison = 0;
        }
        changed = true;
    }

    // The enchantment: fires while charge is left; only the player's weapons run out.
    const auto info = weapon_charge(world, in.weapon, extras.find(in.extra), s, created);
    if (info.enchantment != 0) {
        const auto ench = resolve_enchantment(world, created, info.enchantment, s);
        out.enchantment = info.enchantment;
        out.max = info.max;
        const ChargeSpend spent = spend_charge(extra.charge, info.max, info.cost, in.attacker_is_player);
        out.fired = spent.effect;
        out.charge = spent.after;
        if (spent.effect && in.target_alive && ench) {
            auto inputs = ench->effects;
            for (auto& e : inputs) {
                e.index = -1; // the source is the weapon: its entry conditions cannot be found by index
            }
            const auto ids = effects.apply(in.attacker, in.target, {SourceKind::enchantment, in.weapon, 0}, inputs);
            out.instances.insert(out.instances.end(), ids.begin(), ids.end());
        }
        if (in.attacker_is_player && spent.effect && spent.after != charge_now(extra.charge, info.max)) {
            extra.charge = spent.after;
            changed = true;
        }
    }
    if (changed) {
        if (in.extra_unique && in.extra != 0 && extras.replace(in.extra, extra)) {
            out.new_extra = in.extra; // sole holder: the entry changes, the table does not grow
        } else {
            out.new_extra = extras.intern(extra);
        }
    }
    return out;
}

// ---- worn enchantments ------------------------------------------------------------------------

void WornEnchantments::sync(std::uint32_t actor, const std::vector<WornEnchant>& worn) {
    auto& have = applied_[actor];
    // End what is gone or changed.
    for (auto it = have.begin(); it != have.end();) {
        const auto now = std::ranges::find(worn, it->first, &WornEnchant::item);
        if (now == worn.end() || !(*now == it->second)) {
            effects_.dispel(actor, it->first);
            it = have.erase(it);
        } else {
            ++it;
        }
    }
    // Start what is new.
    for (const WornEnchant& w : worn) {
        if (have.contains(w.item)) {
            continue;
        }
        const auto ench = resolve_enchantment(world_, created_, w.enchantment);
        if (!ench) {
            continue;
        }
        auto inputs = ench->effects;
        for (auto& e : inputs) {
            e.index = -1;
        }
        effects_.apply(actor, actor, {SourceKind::enchantment, w.item, w.extra}, inputs, {.constant = true});
        have[w.item] = w;
    }
    if (have.empty()) {
        applied_.erase(actor);
    }
}

std::vector<WornEnchant> WornEnchantments::applied(std::uint32_t actor) const {
    std::vector<WornEnchant> out;
    const auto it = applied_.find(actor);
    if (it != applied_.end()) {
        for (const auto& [item, w] : it->second) {
            out.push_back(w);
        }
    }
    return out;
}

} // namespace skydot::magic
