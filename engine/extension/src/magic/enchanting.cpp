// SPDX-License-Identifier: GPL-3.0-or-later
#include "magic/enchanting.hpp"

#include "data/fb_search.hpp"
#include "vm/crafting.hpp"

#include <algorithm>
#include <cmath>

namespace skydot::magic {

namespace {

constexpr double k_cost_exponent = 1.1;

const wfb::ItemData* item_of(const wfb::World& world, std::uint32_t form) {
    const auto* base = lookup(world.bases(), form);
    return base != nullptr ? base->item() : nullptr;
}

std::string name_of(const wfb::World& world, std::uint32_t form) {
    const auto* base = lookup(world.bases(), form);
    return base != nullptr && base->name() != nullptr ? base->name()->str() : std::string{};
}

} // namespace

double enchant_skill_multiplier(double skill) {
    const double s = skill / 100.0;
    return 1.0 + s * (s - 0.14) / 3.4;
}

double soul_charge(std::uint8_t soul) {
    constexpr double table[] = {0.0, 250.0, 500.0, 1000.0, 2000.0, 3000.0};
    return soul >= 1 && soul <= 5 ? table[soul] : 0.0;
}

double soul_magnitude_multiplier(std::uint8_t soul) {
    constexpr double table[] = {0.0, 1.0 / 12.0, 1.0 / 6.0, 1.0 / 3.0, 2.0 / 3.0, 1.0};
    return soul >= 1 && soul <= 5 ? table[soul] : 0.0;
}

double weapon_use_cost(double base_cost, double skill) {
    if (base_cost <= 0.0) {
        return 0.0;
    }
    return 3.0 * std::pow(base_cost, k_cost_exponent) * (1.0 - std::sqrt(skill / 200.0));
}

vm::CreatedEffect scale_effect(const EffectInput& base, std::uint32_t mgef_flags, double factor) {
    vm::CreatedEffect out{base.effect, base.magnitude, base.duration, base.area};
    const bool duration = (mgef_flags & crafting::k_mgef_power_affects_duration) != 0 &&
                          (mgef_flags & crafting::k_mgef_power_affects_magnitude) == 0;
    if (duration) {
        if (base.duration > 0) {
            out.duration = static_cast<std::uint32_t>(std::max(1.0, std::floor(static_cast<double>(base.duration) * factor)));
        }
    } else if (base.magnitude > 0.0F) {
        out.magnitude = static_cast<float>(std::max(1.0, std::floor(static_cast<double>(base.magnitude) * factor)));
    }
    return out;
}

ActorSoul actor_soul(std::uint32_t level, bool is_npc) {
    if (is_npc) {
        return {k_soul_grand, true};
    }
    std::uint8_t size = 1;
    if (level >= 38) {
        size = 5;
    } else if (level >= 28) {
        size = 4;
    } else if (level >= 16) {
        size = 3;
    } else if (level >= 4) {
        size = 2;
    }
    return {size, false};
}

ItemKind item_kind(const wfb::World& world, std::uint32_t form) {
    const auto* item = item_of(world, form);
    if (item == nullptr) {
        return ItemKind::none;
    }
    switch (item->category()) {
    case wfb::ItemCategory::weapon:
        if (item->weapon_type() == 8) {
            return ItemKind::staff;
        }
        return item->weapon_type() == 0 ? ItemKind::none : ItemKind::weapon;
    case wfb::ItemCategory::armor:
    case wfb::ItemCategory::clothing:
    case wfb::ItemCategory::jewelry:
        return ItemKind::apparel;
    default:
        return ItemKind::none;
    }
}

bool base_has_keyword(const wfb::World& world, std::uint32_t form, std::uint32_t keyword) {
    const auto* base = lookup(world.bases(), form);
    if (keyword == 0 || base == nullptr || base->keywords() == nullptr) {
        return false;
    }
    return std::ranges::find(*base->keywords(), keyword) != base->keywords()->end();
}

GemInfo gem_info(const wfb::World& world, const EnchantKeywords& kw, std::uint32_t form, const vm::ItemExtra* extra) {
    GemInfo out;
    const auto* item = item_of(world, form);
    if (item == nullptr || item->category() != wfb::ItemCategory::soul_gem) {
        return out;
    }
    out.gem = true;
    out.soul = item->soul() != 0 ? item->soul() : (extra != nullptr ? extra->soul : std::uint8_t{0});
    out.capacity = item->soul_capacity();
    out.linked = item->soul_gem_linked();
    out.npc = item->holds_npc_souls();
    out.reusable = base_has_keyword(world, form, kw.reusable_soul_gem);
    return out;
}

bool enchantment_fits(const wfb::World& world, const wfb::Enchantment& ench, std::uint32_t form) {
    if (ench.enchantment_type() != k_ench_type_item) {
        return false;
    }
    const ItemKind kind = item_kind(world, form);
    if (kind == ItemKind::weapon ? ench.casting_type() != k_ench_casting_fire_and_forget
                                 : (kind != ItemKind::apparel || ench.casting_type() != k_ench_casting_constant)) {
        return false;
    }
    if (ench.worn_restrictions() == 0) {
        return true;
    }
    const auto* list = lookup(world.form_lists(), ench.worn_restrictions());
    if (list == nullptr || list->forms() == nullptr || list->forms()->size() == 0) {
        return true;
    }
    return std::ranges::any_of(*list->forms(), [&](std::uint32_t kw) { return base_has_keyword(world, form, kw); });
}

namespace {

/// MagicDisallowEnchanting on the item or on one of the enchantment's effects.
bool forbidden(const wfb::World& world, const EnchantKeywords& kw, std::uint32_t form, const wfb::Enchantment* ench) {
    if (kw.disallow_enchanting == 0) {
        return false;
    }
    if (base_has_keyword(world, form, kw.disallow_enchanting)) {
        return true;
    }
    if (ench == nullptr || ench->effects() == nullptr) {
        return false;
    }
    for (const auto* e : *ench->effects()) {
        const auto* m = lookup(world.magic_effects(), e->effect());
        if (m != nullptr && m->keywords() != nullptr &&
            std::ranges::find(*m->keywords(), kw.disallow_enchanting) != m->keywords()->end()) {
            return true;
        }
    }
    return false;
}

} // namespace

DisenchantCheck check_disenchant(const wfb::World& world, const EnchantKeywords& kw, const std::set<std::uint32_t>& known,
                                 std::uint32_t form, const vm::ItemExtra* extra) {
    DisenchantCheck out;
    const std::uint32_t id = enchantment_of(world, form, extra);
    if (id == 0) {
        out.refusal = Refusal::no_enchantment;
        return out;
    }
    const auto* ench = lookup(world.enchantments(), id);
    if (ench == nullptr) {
        out.refusal = Refusal::created; // not the world's: one the player made
        return out;
    }
    if (ench->enchantment_type() == k_ench_type_staff || item_kind(world, form) == ItemKind::staff) {
        out.refusal = Refusal::staff;
        return out;
    }
    if (forbidden(world, kw, form, ench)) {
        out.refusal = Refusal::forbidden;
        return out;
    }
    const std::uint32_t learns = ench->base_enchantment() != 0 ? ench->base_enchantment() : id;
    if (known.contains(learns)) {
        out.refusal = Refusal::known;
        return out;
    }
    out.learns = learns;
    return out;
}

std::optional<MadeEnchantment> make_enchantment(const wfb::World& world, const wfb::Enchantment& base_ench, ItemKind kind,
                                                std::uint8_t soul, double skill, const MagicSettings& s) {
    if ((kind != ItemKind::weapon && kind != ItemKind::apparel) || base_ench.effects() == nullptr ||
        base_ench.effects()->size() == 0 || soul < 1 || soul > 5) {
        return std::nullopt;
    }
    MadeEnchantment out;
    out.ench.base = base_ench.id();
    out.ench.name = base_ench.name() != nullptr ? base_ench.name()->str() : std::string{};
    double factor = enchant_skill_multiplier(skill);
    if (kind == ItemKind::apparel) {
        factor *= soul_magnitude_multiplier(soul);
        out.ench.constant = true;
    } else {
        const double capacity = soul_charge(soul);
        const double base_cost = enchantment_cost(world, base_ench, s);
        double cost = weapon_use_cost(base_cost, skill);
        if (cost > capacity) {
            // One use must fit the gem: shrink the magnitude until 3 * (base cost * r)^1.1 * k is the capacity.
            const double k = 1.0 - std::sqrt(skill / 200.0);
            factor *= std::pow(capacity / (3.0 * k), 1.0 / k_cost_exponent) / base_cost;
            cost = capacity;
        }
        out.ench.cost = cost;
        out.charge_max = static_cast<float>(capacity);
    }
    for (const auto& in : inputs_of(base_ench.effects())) {
        const auto* m = lookup(world.magic_effects(), in.effect);
        out.ench.effects.push_back(scale_effect(in, m != nullptr ? m->flags() : 0U, factor));
    }
    return out;
}

// ---- actions -----------------------------------------------------------------------------------

Refusal Enchanting::disenchant(std::uint32_t form, std::uint32_t extra) {
    if (inventory_.count_extra(player_, form, extra) <= 0) {
        return Refusal::no_item;
    }
    const auto check = check_disenchant(world_, kw_, book_.known_enchantments(), form, extras_.find(extra));
    if (check.refusal != Refusal::none) {
        return check.refusal;
    }
    if (!book_.learn_enchantment(check.learns)) {
        return Refusal::unknown_enchantment; // not an ENCH of this world
    }
    (void)inventory_.remove_extra(player_, form, extra, 1);
    return Refusal::none;
}

EnchantOutcome Enchanting::plan(const EnchantRequest& req, MadeEnchantment* made_out) const {
    EnchantOutcome out;
    const auto refuse = [&](Refusal r) {
        out.refusal = r;
        return out;
    };
    if (inventory_.count_extra(player_, req.item, req.item_extra) <= 0) {
        return refuse(Refusal::no_item);
    }
    const ItemKind kind = item_kind(world_, req.item);
    if (kind == ItemKind::staff) {
        return refuse(Refusal::staff);
    }
    if (kind == ItemKind::none) {
        return refuse(Refusal::not_enchantable);
    }
    const auto* extra = extras_.find(req.item_extra);
    if (enchantment_of(world_, req.item, extra) != 0) {
        return refuse(Refusal::enchanted);
    }
    const auto* ench = lookup(world_.enchantments(), req.enchantment);
    if (ench == nullptr || !book_.knows_enchantment(req.enchantment)) {
        return refuse(Refusal::unknown_enchantment);
    }
    if (forbidden(world_, kw_, req.item, ench)) {
        return refuse(Refusal::forbidden);
    }
    if (!enchantment_fits(world_, *ench, req.item)) {
        return refuse(Refusal::does_not_fit);
    }
    if (inventory_.count_extra(player_, req.gem, req.gem_extra) <= 0) {
        return refuse(Refusal::no_gem);
    }
    const GemInfo gem = gem_info(world_, kw_, req.gem, extras_.find(req.gem_extra));
    if (!gem.gem || gem.soul == 0) {
        return refuse(Refusal::no_gem);
    }
    auto made = make_enchantment(world_, *ench, kind, gem.soul, skill_, settings_);
    if (!made) {
        return refuse(Refusal::no_effects);
    }
    out.charge_max = made->charge_max;
    out.cost = made->ench.cost;
    out.name = req.name;
    if (out.name.empty()) {
        std::string suffix = made->ench.name;
        if (suffix.empty() && !made->ench.effects.empty()) {
            const auto* m = lookup(world_.magic_effects(), made->ench.effects.front().effect);
            suffix = m != nullptr && m->name() != nullptr ? m->name()->str() : std::string{};
        }
        out.name = name_of(world_, req.item) + (suffix.empty() ? "" : " of " + suffix);
    }
    if (made_out != nullptr) {
        *made_out = std::move(*made);
    }
    return out;
}

EnchantOutcome Enchanting::preview(const EnchantRequest& req) const { return plan(req, nullptr); }

EnchantOutcome Enchanting::enchant(const EnchantRequest& req) {
    MadeEnchantment made;
    EnchantOutcome out = plan(req, &made);
    if (!out.ok()) {
        return out;
    }
    out.created = created_.create_enchantment(std::move(made.ench));
    if (out.created == 0) {
        out.refusal = Refusal::no_effects; // the created range is used up
        return out;
    }
    vm::ItemExtra changed;
    if (const auto* old = extras_.find(req.item_extra)) {
        changed = *old;
    }
    changed.enchantment = out.created;
    changed.name = out.name;
    if (made.charge_max > 0.0F) {
        changed.charge_max = made.charge_max;
        changed.charge = vm::k_full_charge;
    }
    out.new_extra = extras_.intern(changed);
    (void)inventory_.remove_extra(player_, req.item, req.item_extra, 1);
    inventory_.add(player_, req.item, 1, out.new_extra);
    spend_gem(req.gem, req.gem_extra);
    return out;
}

RechargeOutcome Enchanting::recharge(std::uint32_t item, std::uint32_t item_extra, std::uint32_t gem,
                                     std::uint32_t gem_extra) {
    RechargeOutcome out;
    if (inventory_.count_extra(player_, item, item_extra) <= 0) {
        out.refusal = Refusal::no_item;
        return out;
    }
    if (item_kind(world_, item) != ItemKind::weapon) {
        out.refusal = Refusal::not_enchantable;
        return out;
    }
    const auto* extra = extras_.find(item_extra);
    const auto info = weapon_charge(world_, item, extra, settings_, &created_);
    if (info.enchantment == 0) {
        out.refusal = Refusal::no_enchantment;
        return out;
    }
    out.max = info.max;
    out.charge = info.charge;
    out.new_extra = item_extra;
    if (info.charge >= info.max) {
        out.refusal = Refusal::full;
        return out;
    }
    const GemInfo g = inventory_.count_extra(player_, gem, gem_extra) > 0
                          ? gem_info(world_, kw_, gem, extras_.find(gem_extra))
                          : GemInfo{};
    if (!g.gem || g.soul == 0) {
        out.refusal = Refusal::no_gem;
        return out;
    }
    out.charge = static_cast<float>(std::min(static_cast<double>(info.max), static_cast<double>(info.charge) + soul_charge(g.soul)));
    vm::ItemExtra changed;
    if (extra != nullptr) {
        changed = *extra;
    }
    changed.charge = out.charge;
    out.new_extra = extras_.intern(changed);
    (void)inventory_.remove_extra(player_, item, item_extra, 1);
    inventory_.add(player_, item, 1, out.new_extra);
    spend_gem(gem, gem_extra);
    return out;
}

std::uint32_t Enchanting::empty_form_of(std::uint32_t filled) const {
    const auto* all = world_.bases();
    if (all == nullptr) {
        return 0;
    }
    for (const auto* b : *all) {
        const auto* it = b->item();
        if (it != nullptr && it->category() == wfb::ItemCategory::soul_gem && it->soul() == 0 &&
            it->soul_gem_linked() == filled) {
            return b->id();
        }
    }
    return 0;
}

void Enchanting::spend_gem(std::uint32_t form, std::uint32_t extra) {
    const GemInfo g = gem_info(world_, kw_, form, extras_.find(extra));
    (void)inventory_.remove_extra(player_, form, extra, 1);
    if (!g.reusable) {
        return;
    }
    const auto* item = item_of(world_, form);
    if (item != nullptr && item->soul() != 0) {
        // A filled form: its empty one (the one that links to it), if it has one.
        if (const std::uint32_t empty = empty_form_of(form)) {
            inventory_.add(player_, empty, 1);
        }
        return;
    }
    vm::ItemExtra emptied;
    if (const auto* old = extras_.find(extra)) {
        emptied = *old;
    }
    emptied.soul = 0;
    inventory_.add(player_, form, 1, extras_.intern(emptied));
}

SoulFill Enchanting::fill_gem(ActorSoul soul) {
    SoulFill out;
    if (soul.size < 1 || soul.size > 5) {
        return out;
    }
    struct Candidate {
        std::uint32_t form, extra;
        GemInfo gem;
    };
    std::vector<Candidate> found;
    for (const auto& [form, total] : inventory_.items(player_)) {
        const auto* item = item_of(world_, form);
        if (item == nullptr || item->category() != wfb::ItemCategory::soul_gem) {
            continue;
        }
        std::vector<std::uint32_t> stacks;
        if (inventory_.count_extra(player_, form, 0) > 0) {
            stacks.push_back(0);
        }
        for (const auto& [extra, n] : inventory_.extras_of(player_, form)) {
            stacks.push_back(extra);
        }
        for (const std::uint32_t extra : stacks) {
            const GemInfo g = gem_info(world_, kw_, form, extras_.find(extra));
            if (g.soul == 0 && g.capacity >= soul.size && (!soul.npc || g.npc)) {
                found.push_back({form, extra, g});
            }
        }
    }
    if (found.empty()) {
        return out;
    }
    // The smallest that takes it; a black gem is kept for NPC souls when another fits.
    const auto best = std::ranges::min_element(found, [](const Candidate& a, const Candidate& b) {
        if (a.gem.capacity != b.gem.capacity) {
            return a.gem.capacity < b.gem.capacity;
        }
        if (a.gem.npc != b.gem.npc) {
            return !a.gem.npc;
        }
        return a.form < b.form;
    });
    out.filled = true;
    out.gem = best->form;
    out.soul = soul.size;
    const auto* linked = best->gem.linked != 0 ? item_of(world_, best->gem.linked) : nullptr;
    vm::ItemExtra old;
    if (const auto* e = extras_.find(best->extra)) {
        old = *e;
    }
    (void)inventory_.remove_extra(player_, best->form, best->extra, 1);
    if (linked != nullptr && linked->soul() == soul.size) {
        out.form = best->gem.linked;
        out.extra = extras_.intern(old);
    } else {
        old.soul = soul.size;
        out.form = best->form;
        out.extra = extras_.intern(old);
    }
    inventory_.add(player_, out.form, 1, out.extra);
    return out;
}

} // namespace skydot::magic
