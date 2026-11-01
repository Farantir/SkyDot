// SPDX-License-Identifier: GPL-3.0-or-later
//
// The item domain of the Papyrus runtime (vm/item_runtime.hpp).
#include "vm/item_runtime.hpp"

#include "data/fb_search.hpp"
#include "data/leveled.hpp"
#include "data/start_inventory.hpp"
#include "vm/activation.hpp"
#include "vm/natives_args.hpp"

#include <algorithm>

namespace skydot::vm {

namespace wfb = bethconv::pack::wfb;

ItemInfo ItemRuntime::item_info_of(std::uint32_t form) const {
    ItemInfo info;
    info.form = form;
    const auto* root = host_->world_root();
    const auto* base = root != nullptr ? lookup(root->bases(), form) : nullptr;
    if (base == nullptr) {
        // A potion that alchemy made.
        if (const auto* potion = host_->created_forms().find(form)) {
            info.known = true;
            info.name = potion->name;
            info.category = ItemCategory::potion;
            info.weight = potion->weight;
            info.value = potion->value;
        }
        return info;
    }
    if (base->name() != nullptr) {
        info.name = base->name()->str();
    }
    const auto* item = base->item();
    if (item == nullptr) {
        return info;
    }
    info.known = true;
    info.category = static_cast<ItemCategory>(static_cast<std::uint8_t>(item->category()));
    info.weight = item->weight();
    info.value = item->value();
    info.playable = item->playable();
    info.equip_slot = item->equip_slot();
    info.weapon_type = item->weapon_type();
    info.damage = item->damage();
    info.armor_rating = item->armor_rating();
    info.armor_type = item->armor_type();
    info.biped_slots = item->biped_slots();
    info.food = item->food();
    // World.fb 35: absent (0) in older packs.
    info.soul = item->soul();
    info.soul_capacity = item->soul_capacity();
    info.soul_gem_linked = item->soul_gem_linked();
    info.holds_npc_souls = item->holds_npc_souls();
    // World.fb 38: absent (0, no skill) in older packs.
    info.book_flags = item->book_flags();
    info.book_skill = item->book_skill();
    info.book_spell = item->book_spell();
    return info;
}

void ItemRuntime::resolve_owner(std::uint32_t owner) const {
    if (owner == 0 || states_.resolved(owner)) {
        return;
    }
    states_.mark_resolved(owner);
    const auto* root = host_->world_root();
    // The player's items come from the game's start, not from its record.
    if (root == nullptr || owner == k_player_ref) {
        return;
    }
    const leveled::GlobalValue global = [this](std::uint32_t id) { return host_->global_value_of(id); };
    const std::uint32_t placed = host_->placed_actor_base(owner);
    const std::uint32_t base = placed != 0 ? placed : host_->world_access().base_form(owner);
    if (base == 0) {
        return;
    }
    // An actor: a placed NPC_, or a leveled list of them (the pick is kept).
    std::uint32_t npc = states_.pick_of(owner);
    if (npc == 0) {
        npc = pick_actor_form(*root, base, owner);
        if (npc != base && npc != 0) {
            states_.set_pick(owner, npc);
        }
    }
    auto& inventory = host_->inventory();
    if (npc != 0 && lookup(root->npcs(), npc) != nullptr) {
        const auto contents = actor_contents(*root, npc, owner, global);
        for (const auto& item : contents.items) {
            inventory.add(owner, item.form, item.count);
        }
        auto& equipment = states_.equipment(owner);
        const auto lookup_info = [this](std::uint32_t form) { return item_info_of(form); };
        for (const std::uint32_t form : contents.worn) {
            const auto info = item_info_of(form);
            if (!info.known || (info.category != ItemCategory::armor && info.category != ItemCategory::clothing &&
                                info.category != ItemCategory::jewelry) ||
                is_shield(info)) {
                continue;
            }
            // Two pieces on one slot: the first keeps it, as the builder does.
            const bool taken = std::ranges::any_of(equipment.worn, [&](std::uint32_t worn) {
                return (lookup_info(worn).biped_slots & info.biped_slots) != 0;
            });
            if (!taken) {
                equipment.worn.insert(form);
            }
        }
        // The weapon in the right hand and the shield in the left, as the game equips an NPC
        // that spawns; the bow and arrows stay in the inventory (`ranged_weapon`).
        std::vector<std::uint32_t> forms;
        forms.reserve(contents.items.size());
        for (const auto& item : contents.items) {
            forms.push_back(item.form);
        }
        const auto gear = choose_gear(forms, lookup_info);
        for (const std::uint32_t form : {gear.melee, gear.shield}) {
            if (form != 0) {
                equip(equipment, item_info_of(form), false, lookup_info);
            }
        }
        return;
    }
    for (const auto& item : container_contents(*root, base, owner, leveled::player_level(), global)) {
        inventory.add(owner, item.form, item.count);
    }
}

void ItemRuntime::give_death_item(std::uint32_t ref) {
    const auto* root = host_->world_root();
    if (root == nullptr || ref == k_player_ref) {
        return;
    }
    resolve_owner(ref);
    const std::uint32_t placed = host_->placed_actor_base(ref);
    const std::uint32_t base = placed != 0 ? placed : host_->world_access().base_form(ref);
    const std::uint32_t pick = states_.pick_of(ref);
    const std::uint32_t npc = pick != 0 ? pick : base;
    if (npc == 0 || lookup(root->npcs(), npc) == nullptr) {
        return;
    }
    const leveled::GlobalValue global = [this](std::uint32_t id) { return host_->global_value_of(id); };
    for (const auto& item : death_contents(*root, npc, ref, global)) {
        host_->inventory().add(ref, item.form, item.count);
    }
}

void ItemRuntime::refresh_equipment(std::uint32_t owner, std::uint32_t form) {
    if (states_.find(owner) == nullptr) {
        return;
    }
    auto& equipment = states_.equipment(owner);
    // Each slot that holds `form` needs a unit of its stack left.
    while (equipment.holds(form) &&
           host_->inventory().count_extra(owner, form, equipment.extra_of(form)) <= 0) {
        if (!unequip(equipment, form, equipment.extra_of(form))) {
            break;
        }
    }
    host_->sync_worn_magic(owner);
}

std::vector<Inventory::Taken> ItemRuntime::remove_item_split(std::uint32_t owner, std::uint32_t form,
                                                                   std::int64_t count) {
    resolve_owner(owner);
    auto taken = host_->inventory().remove_split(owner, form, count);
    if (!taken.empty()) {
        refresh_equipment(owner, form);
    }
    return taken;
}

std::int64_t ItemRuntime::remove_item(std::uint32_t owner, std::uint32_t form, std::int64_t count) {
    std::int64_t total = 0;
    for (const auto& part : remove_item_split(owner, form, count)) {
        total += part.count;
    }
    return total;
}

std::int64_t ItemRuntime::remove_item_extra(std::uint32_t owner, std::uint32_t form, std::uint32_t extra,
                                              std::int64_t count) {
    resolve_owner(owner);
    const std::int64_t taken = host_->inventory().remove_extra(owner, form, extra, count);
    if (taken > 0) {
        refresh_equipment(owner, form);
    }
    return taken;
}

std::uint32_t ItemRuntime::resolve_extra(std::uint32_t owner, std::uint32_t form, std::int64_t extra) const {
    if (extra >= 0) {
        return static_cast<std::uint32_t>(extra);
    }
    const auto& inventory = host_->inventory();
    if (inventory.count_extra(owner, form, 0) > 0) {
        return 0;
    }
    const auto& split = inventory.extras_of(owner, form);
    return split.empty() ? 0 : split.begin()->first;
}

bool ItemRuntime::equip_item(std::uint32_t owner, std::uint32_t form, bool left, bool add_missing,
                               std::uint32_t extra) {
    resolve_owner(owner);
    const auto info = item_info_of(form);
    if (!info.known) {
        return false;
    }
    const bool carried = host_->inventory().count_extra(owner, form, extra) > 0;
    if (!carried && !add_missing) {
        return false;
    }
    const auto lookup_info = [this](std::uint32_t id) { return item_info_of(id); };
    Equipment trial = states_.find(owner) != nullptr ? *states_.find(owner) : Equipment{};
    const auto result = equip(trial, info, left, lookup_info, extra);
    if (result.action == EquipAction::refused) {
        return false;
    }
    if (!carried) {
        host_->inventory().add(owner, form, 1, extra);
    }
    switch (result.action) {
    case EquipAction::equipped: {
        const auto before = worn_of(owner);
        const auto held = held_of(owner);
        states_.equipment(owner) = std::move(trial);
        host_->sync_worn_magic(owner);
        note_worn(owner, before);
        note_held(owner, held);
        break;
    }
    case EquipAction::consumed: {
        if (const auto poisoned = host_->apply_consumed(owner, form, extra)) {
            // A poison goes on the weapon in the right hand (it uses itself up); the next hit delivers it.
            return *poisoned;
        }
        remove_item_extra(owner, form, extra, 1);
        if (info.category == ItemCategory::ingredient) {
            // Eating an ingredient teaches its first effect.
            host_->note_eaten(form);
        }
        host_->log("items: " + info.name + " consumed");
        break;
    }
    case EquipAction::read:
        host_->item_trace("item " + info.name + ": reading not implemented");
        host_->log("items: " + info.name + ", reading not implemented");
        break;
    case EquipAction::refused:
        break;
    }
    return true;
}

bool ItemRuntime::unequip_item(std::uint32_t owner, std::uint32_t form) {
    resolve_owner(owner);
    const auto before = worn_of(owner);
    const auto held = held_of(owner);
    const bool done = states_.find(owner) != nullptr && unequip(states_.equipment(owner), form);
    if (done) {
        host_->sync_worn_magic(owner);
        note_worn(owner, before);
        note_held(owner, held);
    }
    return done;
}

void ItemRuntime::unequip_all(std::uint32_t owner) {
    resolve_owner(owner);
    const auto before = worn_of(owner);
    const auto held = held_of(owner);
    states_.clear_equipment(owner);
    host_->sync_worn_magic(owner);
    note_worn(owner, before);
    note_held(owner, held);
}

std::set<std::uint32_t> ItemRuntime::worn_of(std::uint32_t owner) const {
    const auto* equipment = states_.find(owner);
    return equipment != nullptr ? equipment->worn : std::set<std::uint32_t>{};
}

std::set<std::uint32_t> ItemRuntime::held_of(std::uint32_t owner) const {
    std::set<std::uint32_t> out;
    if (const auto* e = states_.find(owner)) {
        out = e->worn;
        for (const auto form : {e->right, e->left, e->ammo}) {
            if (form != 0) {
                out.insert(form);
            }
        }
    }
    return out;
}

void ItemRuntime::send_equip_event(std::uint32_t owner, std::uint32_t form, bool equipped) {
    const Value none;
    host_->dispatch_event(owner, equipped ? "OnObjectEquipped" : "OnObjectUnequipped",
                          {host_->vm().object(form, "Form"), none});
}

void ItemRuntime::note_held(std::uint32_t owner, const std::set<std::uint32_t>& before) {
    const auto after = held_of(owner);
    for (const auto form : before) {
        if (!after.contains(form)) {
            send_equip_event(owner, form, false);
        }
    }
    for (const auto form : after) {
        if (!before.contains(form)) {
            send_equip_event(owner, form, true);
        }
    }
}

void ItemRuntime::note_worn(std::uint32_t owner, const std::set<std::uint32_t>& before) {
    if (worn_of(owner) != before) {
        host_->worn_changed(owner);
    }
}

WornProvider ItemRuntime::worn_provider() const {
    return [this](std::uint32_t ref, std::vector<std::uint32_t>& armors) {
        if (!states_.resolved(ref)) {
            return false;
        }
        if (const auto* equipment = states_.find(ref)) {
            armors.assign(equipment->worn.begin(), equipment->worn.end());
        }
        return true;
    };
}


bool ItemRuntime::unequip_extra(std::uint32_t owner, std::uint32_t form, std::optional<std::uint32_t> extra) {
    resolve_owner(owner);
    if (states_.find(owner) == nullptr) {
        return false;
    }
    const auto held = held_of(owner);
    const bool done = unequip(states_.equipment(owner), form, extra);
    if (done) {
        host_->sync_worn_magic(owner);
        note_held(owner, held);
    }
    return done;
}
std::int64_t ItemRuntime::equipped_item_type(std::uint32_t owner, std::int64_t hand) {
    resolve_owner(owner);
    const auto* equipment = states_.find(owner);
    const std::uint32_t form = equipment == nullptr ? 0 : (hand == 0 ? equipment->left : equipment->right);
    return form == 0 ? 0 : vm::equipped_item_type(item_info_of(form));
}

std::vector<std::uint32_t> ItemRuntime::worn_armor_of(std::uint32_t owner) const {
    resolve_owner(owner);
    std::vector<std::uint32_t> out;
    const auto* equipment = states_.find(owner);
    if (equipment == nullptr) {
        return out;
    }
    const auto wearable = [this](std::uint32_t form) {
        const ItemInfo info = item_info_of(form);
        return info.known && (info.category == ItemCategory::armor || info.category == ItemCategory::clothing);
    };
    for (const std::uint32_t form : equipment->worn) {
        if (wearable(form)) {
            out.push_back(form);
        }
    }
    if (equipment->left != 0 && is_shield(item_info_of(equipment->left))) {
        out.push_back(equipment->left);
    }
    return out;
}

bool ItemRuntime::is_equipped(std::uint32_t owner, std::uint32_t form) const {
    resolve_owner(owner);
    const auto* equipment = states_.find(owner);
    return form != 0 && equipment != nullptr && equipment->holds(form);
}

std::int64_t ItemRuntime::hand_item_type(std::uint32_t owner, std::int64_t hand) const {
    resolve_owner(owner);
    const auto* equipment = states_.find(owner);
    const std::uint32_t form = equipment == nullptr ? 0 : (hand == 0 ? equipment->left : equipment->right);
    return form == 0 ? 0 : vm::equipped_item_type(item_info_of(form));
}

// ---- natives

void ItemRuntime::bind_natives(Vm& vm, Host& host) {
    host_ = &host;
    using natives::arg;
    using natives::result;
    const auto is_object = [](NativeCall& c, std::size_t i) { return arg(c, i).kind() == Kind::object; };

    vm.bind("Actor", "EquipItem", [this, is_object](NativeCall& c) {
        if (is_object(c, 0)) {
            equip_item(c.self.form(), arg(c, 0).form(), false, true);
        }
        return NativeResult{};
    });
    vm.bind("Actor", "UnequipItem", [this, is_object](NativeCall& c) {
        if (is_object(c, 0)) {
            unequip_item(c.self.form(), arg(c, 0).form());
        }
        return NativeResult{};
    });
    vm.bind("Actor", "UnequipAll", [this](NativeCall& c) {
        unequip_all(c.self.form());
        return NativeResult{};
    });
    vm.bind("Actor", "IsEquipped", [this, is_object](NativeCall& c) {
        resolve_owner(c.self.form());
        const auto* equipment = states_.find(c.self.form());
        return result(Value::boolean(is_object(c, 0) && equipment != nullptr && equipment->holds(arg(c, 0).form())));
    });
    // aiHand: 0 left, 1 right.
    vm.bind("Actor", "GetEquippedItemType", [this](NativeCall& c) {
        return result(Value::integer(static_cast<std::int32_t>(equipped_item_type(c.self.form(), arg(c, 0).i()))));
    });
    vm.bind("Actor", "GetEquippedWeapon", [this](NativeCall& c) {
        resolve_owner(c.self.form());
        const auto* equipment = states_.find(c.self.form());
        const std::uint32_t form =
            equipment == nullptr ? 0 : (Vm::truthy(arg(c, 0)) ? equipment->left : equipment->right);
        const auto info = item_info_of(form);
        return form != 0 && info.category == ItemCategory::weapon ? result(c.vm.object(form, "Weapon"))
                                                                     : NativeResult{};
    });
    vm.bind("Actor", "GetEquippedShield", [this](NativeCall& c) {
        resolve_owner(c.self.form());
        const auto* equipment = states_.find(c.self.form());
        const std::uint32_t form = equipment == nullptr ? 0 : equipment->left;
        return form != 0 && is_shield(item_info_of(form)) ? result(c.vm.object(form, "Armor"))
                                                              : NativeResult{};
    });
}

} // namespace skydot::vm
