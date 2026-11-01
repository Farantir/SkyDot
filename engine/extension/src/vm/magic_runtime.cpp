// SPDX-License-Identifier: GPL-3.0-or-later
//
// The magic domain of the Papyrus runtime (vm/magic_runtime.hpp): events, ticks, deaths and the save
// section.
#include "vm/magic_runtime.hpp"

namespace skydot::vm {

namespace {

/// The log word of a held archetype ("" for the others); fear names its source.
std::string held_state_word(const magic::EffectView& v) {
    switch (v.archetype) {
    case magic::archetype::calm: return "calm";
    case magic::archetype::demoralize:
    case magic::archetype::turn_undead: return "fear from " + magic_hex(v.caster);
    case magic::archetype::frenzy: return "frenzy";
    case magic::archetype::paralysis: return "paralyzed";
    case magic::archetype::invisibility: return "invisible";
    case magic::archetype::slow_time: return "slow time " + magic_number(magic::slow_time_scale(v.magnitude));
    default: return {};
    }
}

/// MGEF archetype 23.
constexpr std::uint32_t k_archetype_soul_trap = 23;

} // namespace

void MagicRuntime::flush() {
    if (flushing) {
        return;
    }
    flushing = true;
    while (!pending.empty()) {
        auto batch = std::move(pending);
        pending.clear();
        for (const auto& event : batch) {
            const auto& v = event.view;
            host_->effect_event(v, event.started);
            if (const std::string word = held_state_word(v); !word.empty()) {
                log("magic: " + magic_hex(v.target) + " " + word + (event.started ? " starts" : " ends"));
            }
            if (event.started && v.archetype == magic::archetype::stagger) {
                log("magic: " + magic_hex(v.target) + " staggered (" + magic_number(v.magnitude) + ")");
            }
            const auto* mgef = lookup(world_.magic_effects(), v.mgef);
            const auto* scripts = mgef != nullptr ? mgef->scripts() : nullptr;
            if (scripts == nullptr || scripts->size() == 0) {
                continue;
            }
            // The script object of this effect: its handle is the instance id.
            snapshots[v.id] = v;
            auto& vm = host_->vm();
            const Value target = vm.object(v.target, "Actor");
            const Value caster = vm.object(v.caster, "Actor");
            if (event.started) {
                host_->attach_effect_scripts(v.id, v.mgef);
                vm.send_event(v.id, "OnEffectStart", {target, caster});
            } else {
                vm.send_event(v.id, "OnEffectFinish", {target, caster});
                detach_later.add(v.id);
            }
        }
    }
    flushing = false;
}

void MagicRuntime::tick(double seconds) {
    auto& vm = host_->vm();
    effects.tick(seconds);
    for (const std::uint32_t id : effects.take_forgotten()) { // forgotten while the effects ran
        vm.detach(id);
        snapshots.erase(id);
    }
    shouts.tick(seconds); // real seconds (docs/adr/0008)
    flush();
    // Script objects of finished effects go once their OnEffectFinish had its updates (VM seconds).
    for (const std::uint32_t id : detach_later.tick(seconds)) {
        vm.detach(id);
        snapshots.erase(id);
    }
}

bool MagicRuntime::soul_trapped_by_player(std::uint32_t ref) const {
    if (ref == k_player_ref) {
        return false;
    }
    const auto views = effects.effects_on(ref);
    // The spell's own effect is a Script archetype with Papyrus scripts (SoulTrapFFActor); the
    // enchantments' and perks' are archetype 23. Either, cast by the player, is a Soul Trap.
    const auto soul_trap = [&](const magic::EffectView& v) {
        if (v.caster != k_player_ref) {
            return false;
        }
        if (v.archetype == k_archetype_soul_trap) {
            return true;
        }
        const auto* mgef = lookup(world_.magic_effects(), v.mgef);
        return mgef != nullptr && mgef->editor_id() != nullptr && mgef->editor_id()->str().starts_with("SoulTrap");
    };
    return std::ranges::any_of(views, soul_trap);
}

void MagicRuntime::actor_died(std::uint32_t ref) {
    effects.on_death(ref);
    worn.forget(ref);
    flush();
}

std::int64_t MagicRuntime::break_invisibility(std::uint32_t ref) {
    const auto n = effects.break_invisibility(ref);
    if (n != 0) {
        log("magic: invisibility of " + magic_hex(ref) + " broken");
    }
    flush();
    return static_cast<std::int64_t>(n);
}

bool MagicRuntime::dispel_bound_weapon(std::uint32_t actor, std::uint32_t weapon) {
    const bool any = effects.dispel_bound(actor, weapon) != 0;
    flush();
    return any;
}

void MagicRuntime::clear() {
    effects.clear();
    book.clear();
    shouts.clear();
    worn.clear();
    pending.clear();
    snapshots.clear();
    detach_later.clear();
}

std::vector<std::uint8_t> MagicRuntime::save() const { return magic::write_magc(effects, book, shouts); }

bool MagicRuntime::decode(const wfb::World& world, save::ByteReader r, std::vector<std::uint8_t>& effects,
                          std::vector<std::uint8_t>& book, std::vector<std::uint8_t>& shouts) {
    magic::MagcParts parts;
    if (!magic::read_magc(r, parts) || !magic::check_magc(world, parts)) {
        return false;
    }
    effects = std::move(parts.effects);
    book = std::move(parts.book);
    shouts = std::move(parts.shouts);
    return true;
}

void MagicRuntime::adopt(bool has, const std::vector<std::uint8_t>& saved_effects,
                         const std::vector<std::uint8_t>& saved_book, const std::vector<std::uint8_t>& saved_shouts) {
    clear();
    if (has) {
        // The actor values were replaced without their temporary part: the effects hold it again.
        (void)magic::SpellBook::load(save::ByteReader(saved_book), book);
        // A version 1 section has no shout part: the book stays empty.
        if (!saved_shouts.empty()) {
            (void)magic::ShoutBook::load(save::ByteReader(saved_shouts), shouts);
        }
        (void)effects.load(save::ByteReader(saved_effects));
    }
    // The VM was restored first: script objects of effects that are over (finished before the save, or not in the
    // loaded effects) have nothing to belong to; they get their grace for a handler under way, then go.
    for (const std::uint32_t form : host_->vm().attached_forms()) {
        if (id_ranges::effect_instances.contains(form) && !effects.instance(form)) {
            detach_later.add(form);
        }
    }
    // Abilities are not saved: start them again for whoever is known.
    book.reset_abilities();
    std::vector<std::uint32_t> wearers;
    for (const auto& [owner, equipment] : items_.states().equipments()) {
        if (!equipment.worn.empty() || equipment.left != 0) {
            wearers.push_back(owner);
        }
    }
    const auto refs = magic::actors_to_rederive(book, effects, wearers);
    for (const auto ref : refs) {
        prime(ref);
    }
}

void MagicRuntime::sync_worn_magic(std::uint32_t owner) {
    sync_worn(owner);
    flush();
}

void MagicRuntime::move_unit_extra(std::uint32_t owner, std::uint32_t form, std::uint32_t from, std::uint32_t to) {
    if (from == to || owner == 0) {
        return;
    }
    auto& inventory = host_->inventory();
    if (inventory.remove_extra(owner, form, from, 1) > 0) {
        inventory.add(owner, form, 1, to);
    }
    retarget_equipment(owner, form, from, to);
}

void MagicRuntime::retarget_equipment(std::uint32_t owner, std::uint32_t form, std::uint32_t from, std::uint32_t to) {
    if (from == to || owner == 0 || items_.states().find(owner) == nullptr) {
        return;
    }
    auto& equipment = items_.states().equipment(owner);
    if (equipment.right == form && equipment.right_extra == from) {
        equipment.right_extra = to;
    } else if (equipment.left == form && equipment.left_extra == from) {
        equipment.left_extra = to;
    } else if (equipment.ammo == form && equipment.ammo_extra == from) {
        equipment.ammo_extra = to;
    } else if (equipment.worn.contains(form) && equipment.worn_extra_of(form) == from) {
        if (to == 0) {
            equipment.worn_extra.erase(form);
        } else {
            equipment.worn_extra[form] = to;
        }
    }
}

MagicRuntime::HitMagic MagicRuntime::hit_magic(std::uint32_t attacker, std::uint32_t target, std::uint32_t weapon,
                                               std::uint32_t extra, bool target_alive) {
    HitMagic out;
    if (weapon == 0) {
        return out;
    }
    prime(attacker);
    prime(target);
    magic::HitMagicInput in;
    in.attacker = attacker;
    in.target = target;
    in.weapon = weapon;
    in.extra = extra;
    in.attacker_is_player = attacker == k_player_ref;
    in.target_alive = target_alive;
    in.extra_unique = extra != 0 && host_->inventory().units_with_extra(extra) == 1 && !items_.states().extra_dropped(extra);
    const auto poison_inputs = [this](std::uint32_t poison, std::vector<magic::EffectInput>& inputs) {
        magic::SourceKind kind = magic::SourceKind::potion;
        bool is_poison = false;
        return item_effects(poison, inputs, kind, is_poison);
    };
    const auto result = magic::hit_magic(world_, effects, items_.states().extras(), in, poison_inputs, settings(),
                                         &host_->created_forms());
    if (result.new_extra != extra) {
        move_unit_extra(attacker, weapon, extra, result.new_extra);
    }
    flush();
    out.poison = result.poison;
    out.enchantment = result.enchantment;
    out.fired = result.fired;
    out.charge = static_cast<double>(result.charge);
    out.charge_max = static_cast<double>(result.max);
    if (result.enchantment != 0 && attacker == k_player_ref) {
        log("magic: " + magic_hex(weapon) + " enchantment " + magic_hex(result.enchantment) +
            (result.fired ? " fires" : " has no charge") + " (charge " + magic_number(static_cast<double>(result.charge)) +
            "/" + magic_number(static_cast<double>(result.max)) + ")");
    }
    return out;
}

MagicRuntime::PoisonResult MagicRuntime::poison_weapon(std::uint32_t actor, std::uint32_t form, std::int64_t extra) {
    PoisonResult out;
    if (!host_->actor_states().is_actor(actor)) {
        out.reason = "no actor";
        return out;
    }
    std::vector<magic::EffectInput> inputs;
    magic::SourceKind kind = magic::SourceKind::potion;
    bool poison = false;
    if (!item_effects(form, inputs, kind, poison) || !poison) {
        out.reason = "not a poison";
        return out;
    }
    items_.resolve_owner(actor);
    const std::uint32_t poison_extra = items_.resolve_extra(actor, form, extra);
    if (host_->inventory().count_extra(actor, form, poison_extra) <= 0) {
        out.reason = "not carried";
        return out;
    }
    const Equipment* equipment = items_.states().find(actor);
    const std::uint32_t weapon = equipment != nullptr ? equipment->right : 0;
    const ItemInfo info = items_.item_info_of(weapon);
    // UESP: a poison goes on the weapon in the right hand, a bow included; a staff or fists have no blade.
    if (weapon == 0 || !info.known || info.category != ItemCategory::weapon || info.weapon_type == 0 ||
        info.weapon_type == 8) {
        out.reason = "no weapon";
        return out;
    }
    const std::uint32_t weapon_extra = equipment->right_extra;
    ItemExtra changed;
    if (const auto* now = items_.states().extras().find(weapon_extra)) {
        changed = *now;
    }
    if (!magic::can_poison(changed)) {
        out.reason = "already poisoned";
        return out;
    }
    (void)items_.remove_item_extra(actor, form, poison_extra, 1);
    const std::uint32_t after = items_.states().extras().intern(magic::poisoned(changed, form, magic::k_placeholder_poison_doses));
    move_unit_extra(actor, weapon, weapon_extra, after);
    log("magic: poison " + magic_hex(form) + " put on " + magic_hex(weapon) + " of " + magic_hex(actor));
    out.ok = true;
    out.weapon = weapon;
    out.doses = magic::k_placeholder_poison_doses;
    return out;
}

MagicRuntime::WeaponCharge MagicRuntime::weapon_charge(std::uint32_t actor, std::uint32_t form, std::int64_t extra) {
    items_.resolve_owner(actor);
    std::uint32_t unit = 0;
    const Equipment* equipment = items_.states().find(actor);
    if (extra >= 0) {
        unit = static_cast<std::uint32_t>(extra);
    } else if (equipment != nullptr && equipment->right == form) {
        unit = equipment->right_extra;
    } else {
        unit = items_.resolve_extra(actor, form, -1);
    }
    const auto info = magic::weapon_charge(world_, form, items_.states().extras().find(unit), settings(),
                                           &host_->created_forms());
    WeaponCharge out;
    out.enchantment = info.enchantment;
    out.charge = static_cast<double>(info.charge);
    out.max = static_cast<double>(info.max);
    out.cost = info.cost;
    return out;
}

void MagicRuntime::bind_natives(Vm& vm) {
    bind_spell_natives(vm, world_, *this);
    bind_magiceffect_natives(vm, world_);
    bind_activemagiceffect_natives(vm, *this);
    bind_actor_magic_natives(vm, *this);
    bind_shout_natives(vm, world_);
}

} // namespace skydot::vm
