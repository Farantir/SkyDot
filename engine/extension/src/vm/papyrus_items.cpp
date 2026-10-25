// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's items: start inventories resolved on first touch, what
// owners hold and wear, and the natives that equip. The rules are
// vm/equipment.hpp (pure), the data data/start_inventory.hpp and
// data/leveled.hpp (pure); this is the glue to the VM's inventory, the
// world's records and Godot.
#include "vm/papyrus.hpp"

#include "data/fb_search.hpp"
#include "data/leveled.hpp"
#include "data/start_inventory.hpp"
#include "vm/natives_common.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstdio>

using godot::Array;
using godot::Dictionary;
using godot::String;

namespace skydot {

namespace {

namespace wfb = bethconv::pack::wfb;

String to_godot(const std::string& s) {
    return String::utf8(s.c_str(), static_cast<int>(s.size()));
}

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

vm::ItemInfo SkydotPapyrus::item_info_of(std::uint32_t form) const {
    vm::ItemInfo info;
    info.form = form;
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    const auto* base = root != nullptr ? lookup(root->bases(), form) : nullptr;
    if (base == nullptr) {
        // A potion that alchemy made.
        if (const auto* potion = created_.find(form)) {
            info.known = true;
            info.name = potion->name;
            info.category = vm::ItemCategory::potion;
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
    info.category = static_cast<vm::ItemCategory>(static_cast<std::uint8_t>(item->category()));
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
    return info;
}

void SkydotPapyrus::resolve_owner(std::uint32_t owner) const {
    if (owner == 0 || items_.resolved(owner)) {
        return;
    }
    items_.mark_resolved(owner);
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    // The player's items come from the game's start, not from its record.
    if (root == nullptr || owner == k_player_ref) {
        return;
    }
    const leveled::GlobalValue global = [this](std::uint32_t id) -> std::optional<double> {
        return get_global_value(id);
    };
    const auto* actor = world_->data().actor_ptr(owner);
    const std::uint32_t base = actor != nullptr ? actor->base() : base_form_of(owner);
    if (base == 0) {
        return;
    }
    // An actor: a placed NPC_, or a leveled list of them (the pick is kept).
    std::uint32_t npc = items_.pick_of(owner);
    if (npc == 0) {
        npc = pick_actor_form(*root, base, owner);
        if (npc != base && npc != 0) {
            items_.set_pick(owner, npc);
        }
    }
    auto& inventory = vm_->inventory();
    if (npc != 0 && lookup(root->npcs(), npc) != nullptr) {
        const auto contents = actor_contents(*root, npc, owner, global);
        for (const auto& item : contents.items) {
            inventory.add(owner, item.form, item.count);
        }
        auto& equipment = items_.equipment(owner);
        const auto lookup_info = [this](std::uint32_t form) { return item_info_of(form); };
        for (const std::uint32_t form : contents.worn) {
            const auto info = item_info_of(form);
            if (!info.known || (info.category != vm::ItemCategory::armor && info.category != vm::ItemCategory::clothing &&
                                info.category != vm::ItemCategory::jewelry) ||
                vm::is_shield(info)) {
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
        const auto gear = vm::choose_gear(forms, lookup_info);
        for (const std::uint32_t form : {gear.melee, gear.shield}) {
            if (form != 0) {
                vm::equip(equipment, item_info_of(form), false, lookup_info);
            }
        }
        return;
    }
    for (const auto& item : container_contents(*root, base, owner, leveled::player_level(), global)) {
        inventory.add(owner, item.form, item.count);
    }
}

void SkydotPapyrus::give_death_item(std::uint32_t ref) {
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    if (root == nullptr || ref == k_player_ref) {
        return;
    }
    resolve_owner(ref);
    const auto* actor = world_->data().actor_ptr(ref);
    const std::uint32_t base = actor != nullptr ? actor->base() : base_form_of(ref);
    const std::uint32_t pick = items_.pick_of(ref);
    const std::uint32_t npc = pick != 0 ? pick : base;
    if (npc == 0 || lookup(root->npcs(), npc) == nullptr) {
        return;
    }
    const leveled::GlobalValue global = [this](std::uint32_t id) -> std::optional<double> {
        return get_global_value(id);
    };
    for (const auto& item : death_contents(*root, npc, ref, global)) {
        vm_->inventory().add(ref, item.form, item.count);
    }
}

void SkydotPapyrus::refresh_equipment(std::uint32_t owner, std::uint32_t form) {
    if (items_.find(owner) == nullptr) {
        return;
    }
    auto& equipment = items_.equipment(owner);
    // Each slot that holds `form` needs a unit of its stack left.
    while (equipment.holds(form) &&
           vm_->inventory().count_extra(owner, form, equipment.extra_of(form)) <= 0) {
        if (!vm::unequip(equipment, form, equipment.extra_of(form))) {
            break;
        }
    }
    sync_worn_magic(owner);
}

std::vector<vm::Inventory::Taken> SkydotPapyrus::remove_item_split(std::uint32_t owner, std::uint32_t form,
                                                                   std::int64_t count) {
    resolve_owner(owner);
    auto taken = vm_->inventory().remove_split(owner, form, count);
    if (!taken.empty()) {
        refresh_equipment(owner, form);
    }
    return taken;
}

std::int64_t SkydotPapyrus::remove_item(std::uint32_t owner, std::uint32_t form, std::int64_t count) {
    std::int64_t total = 0;
    for (const auto& part : remove_item_split(owner, form, count)) {
        total += part.count;
    }
    return total;
}

std::int64_t SkydotPapyrus::remove_item_extra(std::uint32_t owner, std::uint32_t form, std::uint32_t extra,
                                              std::int64_t count) {
    resolve_owner(owner);
    const std::int64_t taken = vm_->inventory().remove_extra(owner, form, extra, count);
    if (taken > 0) {
        refresh_equipment(owner, form);
    }
    return taken;
}

std::uint32_t SkydotPapyrus::resolve_extra(std::uint32_t owner, std::uint32_t form, std::int64_t extra) const {
    if (extra >= 0) {
        return static_cast<std::uint32_t>(extra);
    }
    const auto& inventory = vm_->inventory();
    if (inventory.count_extra(owner, form, 0) > 0) {
        return 0;
    }
    const auto& split = inventory.extras_of(owner, form);
    return split.empty() ? 0 : split.begin()->first;
}

bool SkydotPapyrus::equip_item(std::uint32_t owner, std::uint32_t form, bool left, bool add_missing,
                               std::uint32_t extra) {
    resolve_owner(owner);
    const auto info = item_info_of(form);
    if (!info.known) {
        return false;
    }
    const bool carried = vm_->inventory().count_extra(owner, form, extra) > 0;
    if (!carried && !add_missing) {
        return false;
    }
    const auto lookup_info = [this](std::uint32_t id) { return item_info_of(id); };
    vm::Equipment trial = items_.find(owner) != nullptr ? *items_.find(owner) : vm::Equipment{};
    const auto result = vm::equip(trial, info, left, lookup_info, extra);
    if (result.action == vm::EquipAction::refused) {
        return false;
    }
    if (!carried) {
        vm_->inventory().add(owner, form, 1, extra);
    }
    switch (result.action) {
    case vm::EquipAction::equipped: {
        const auto before = worn_of(owner);
        items_.equipment(owner) = std::move(trial);
        sync_worn_magic(owner);
        note_worn(owner, before);
        break;
    }
    case vm::EquipAction::consumed: {
        const auto applied = apply_item_effects(owner, form, extra);
        if (String(applied.get("reason", String())) == "poison") {
            // A poison goes on the weapon in the right hand (it uses itself up); the next hit delivers it.
            return bool(poison_weapon(owner, form, static_cast<std::int64_t>(extra)).get("ok", false));
        }
        remove_item_extra(owner, form, extra, 1);
        if (info.category == vm::ItemCategory::ingredient) {
            // Eating an ingredient teaches its first effect.
            if (auto* logic = crafting_logic()) {
                logic->note_eaten(form);
            }
        }
        godot::UtilityFunctions::print(to_godot("items: " + info.name + " consumed"));
        break;
    }
    case vm::EquipAction::read:
        emit_signal("trace", to_godot("item " + info.name + ": reading not implemented"));
        godot::UtilityFunctions::print(to_godot("items: " + info.name + ", reading not implemented"));
        break;
    case vm::EquipAction::refused:
        break;
    }
    return true;
}

bool SkydotPapyrus::unequip_item(std::uint32_t owner, std::uint32_t form) {
    resolve_owner(owner);
    const auto before = worn_of(owner);
    const bool done = items_.find(owner) != nullptr && vm::unequip(items_.equipment(owner), form);
    if (done) {
        sync_worn_magic(owner);
        note_worn(owner, before);
    }
    return done;
}

std::string SkydotPapyrus::item_name_of(std::uint32_t form) const {
    return item_info_of(form).name;
}

void SkydotPapyrus::unequip_all(std::uint32_t owner) {
    resolve_owner(owner);
    const auto before = worn_of(owner);
    items_.clear_equipment(owner);
    sync_worn_magic(owner);
    note_worn(owner, before);
}

std::set<std::uint32_t> SkydotPapyrus::worn_of(std::uint32_t owner) const {
    const auto* equipment = items_.find(owner);
    return equipment != nullptr ? equipment->worn : std::set<std::uint32_t>{};
}

void SkydotPapyrus::note_worn(std::uint32_t owner, const std::set<std::uint32_t>& before) {
    if (worn_of(owner) != before) {
        emit_signal("worn_changed", static_cast<std::int64_t>(owner));
    }
}

WornProvider SkydotPapyrus::worn_provider() const {
    return [this](std::uint32_t ref, std::vector<std::uint32_t>& armors) {
        if (!items_.resolved(ref)) {
            return false;
        }
        if (const auto* equipment = items_.find(ref)) {
            armors.assign(equipment->worn.begin(), equipment->worn.end());
        }
        return true;
    };
}

// ---- the game layer's view ------------------------------------------------------

void SkydotPapyrus::resolve(std::int64_t owner) {
    resolve_owner(static_cast<std::uint32_t>(owner));
}

Dictionary SkydotPapyrus::item_info(std::int64_t form) const {
    const auto info = item_info_of(static_cast<std::uint32_t>(form));
    Dictionary out;
    if (!info.known) {
        return out;
    }
    out["found"] = true;
    out["form"] = form;
    out["name"] = to_godot(info.name);
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    const auto* base = root != nullptr ? lookup(root->bases(), static_cast<std::uint32_t>(form)) : nullptr;
    out["editor_id"] = base != nullptr && base->editor_id() != nullptr ? to_godot(base->editor_id()->str()) : String();
    out["category"] = vm::category_name(info.category);
    out["weight"] = static_cast<double>(info.weight);
    out["value"] = static_cast<std::int64_t>(info.value);
    out["playable"] = info.playable;
    out["equip_slot"] = static_cast<std::int64_t>(info.equip_slot);
    out["weapon_type"] = static_cast<std::int64_t>(info.weapon_type);
    out["damage"] = static_cast<double>(info.damage);
    out["armor_rating"] = static_cast<double>(info.armor_rating);
    out["armor_type"] = static_cast<std::int64_t>(info.armor_type);
    out["biped_slots"] = static_cast<std::int64_t>(info.biped_slots);
    out["soul"] = static_cast<std::int64_t>(info.soul);
    out["soul_capacity"] = static_cast<std::int64_t>(info.soul_capacity);
    out["soul_gem_linked"] = static_cast<std::int64_t>(info.soul_gem_linked);
    out["holds_npc_souls"] = info.holds_npc_souls;
    return out;
}

Array SkydotPapyrus::inventory(std::int64_t owner) {
    const auto id = static_cast<std::uint32_t>(owner);
    resolve_owner(id);
    const auto* equipment = items_.find(id);
    struct Row {
        std::string key;
        std::uint32_t form;
        std::uint32_t extra;
        std::int64_t count;
        std::string name;
        const char* tier;
        vm::ItemInfo info;
    };
    std::vector<Row> rows;
    const auto& inventory = vm_->inventory();
    for (const auto& [form, total] : inventory.items(id)) {
        auto info = item_info_of(form);
        const auto add_row = [&](std::uint32_t extra, std::int64_t count) {
            std::string name = info.name;
            const char* tier = "";
            if (const auto* e = items_.extras().find(extra)) {
                tier = vm::temper_tier_name(e->health);
                if (!e->name.empty()) {
                    name = e->name;
                }
                if (tier[0] != '\0') {
                    name += std::string(" (") + tier + ")";
                }
            }
            rows.push_back({lower(name), form, extra, count, std::move(name), tier, info});
        };
        if (const std::int64_t plain = inventory.count_extra(id, form, 0); plain > 0) {
            add_row(0, plain);
        }
        for (const auto& [extra, count] : inventory.extras_of(id, form)) {
            add_row(extra, count);
        }
    }
    std::ranges::sort(rows, [](const Row& a, const Row& b) {
        if (a.key != b.key) {
            return a.key < b.key;
        }
        return a.form != b.form ? a.form < b.form : a.extra < b.extra;
    });
    Array out;
    for (const auto& row : rows) {
        Dictionary entry;
        entry["form"] = static_cast<std::int64_t>(row.form);
        entry["extra"] = static_cast<std::int64_t>(row.extra);
        entry["name"] = to_godot(row.name);
        entry["tier"] = to_godot(row.tier);
        entry["count"] = row.count;
        entry["weight"] = static_cast<double>(row.info.weight);
        entry["value"] = static_cast<std::int64_t>(row.info.value);
        entry["category"] = vm::category_name(row.info.category);
        entry["equipped"] = equipment != nullptr && equipment->holds(row.form, row.extra);
        entry["playable"] = row.info.playable;
        // An enchanted weapon shows its charge, a filled soul gem its soul.
        const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
        if (root != nullptr && row.info.category == vm::ItemCategory::weapon) {
            const auto charge = magic::weapon_charge(*root, row.form, items_.extras().find(row.extra), {}, &created_);
            if (charge.enchantment != 0 && charge.max > 0.0F) {
                entry["charge"] = static_cast<double>(charge.charge);
                entry["charge_max"] = static_cast<double>(charge.max);
            }
        } else if (root != nullptr && row.info.category == vm::ItemCategory::soul_gem) {
            const auto gem = magic::gem_info(*root, {}, row.form, items_.extras().find(row.extra));
            entry["soul"] = static_cast<std::int64_t>(gem.soul);
            entry["soul_capacity"] = static_cast<std::int64_t>(gem.capacity);
        }
        out.push_back(entry);
    }
    return out;
}

std::int64_t SkydotPapyrus::transfer(std::int64_t from, std::int64_t to, std::int64_t form, std::int64_t count,
                                     std::int64_t extra) {
    const auto source = static_cast<std::uint32_t>(from);
    const auto target = static_cast<std::uint32_t>(to);
    const auto base = static_cast<std::uint32_t>(form);
    resolve_owner(source);
    resolve_owner(target);
    std::int64_t moved = 0;
    if (extra < 0) {
        for (const auto& part : remove_item_split(source, base, count)) {
            vm_->inventory().add(target, base, part.count, part.extra);
            moved += part.count;
        }
    } else {
        moved = remove_item_extra(source, base, static_cast<std::uint32_t>(extra), count);
        vm_->inventory().add(target, base, moved, static_cast<std::uint32_t>(extra));
    }
    return moved;
}

bool SkydotPapyrus::equip(std::int64_t owner, std::int64_t form, std::int64_t extra) {
    const auto id = static_cast<std::uint32_t>(owner);
    const auto base = static_cast<std::uint32_t>(form);
    resolve_owner(id);
    return equip_item(id, base, false, false, resolve_extra(id, base, extra));
}

bool SkydotPapyrus::unequip(std::int64_t owner, std::int64_t form, std::int64_t extra) {
    resolve_owner(static_cast<std::uint32_t>(owner));
    const auto id = static_cast<std::uint32_t>(owner);
    if (items_.find(id) == nullptr) {
        return false;
    }
    const bool done = vm::unequip(items_.equipment(id), static_cast<std::uint32_t>(form),
                                  extra < 0 ? std::nullopt : std::optional<std::uint32_t>(static_cast<std::uint32_t>(extra)));
    if (done) {
        sync_worn_magic(id);
    }
    return done;
}

Dictionary SkydotPapyrus::equipment(std::int64_t owner) {
    const auto id = static_cast<std::uint32_t>(owner);
    resolve_owner(id);
    Dictionary out;
    const auto* equipment = items_.find(id);
    out["right"] = static_cast<std::int64_t>(equipment != nullptr ? equipment->right : 0);
    out["left"] = static_cast<std::int64_t>(equipment != nullptr ? equipment->left : 0);
    out["ammo"] = static_cast<std::int64_t>(equipment != nullptr ? equipment->ammo : 0);
    out["right_extra"] = static_cast<std::int64_t>(equipment != nullptr ? equipment->right_extra : 0);
    out["left_extra"] = static_cast<std::int64_t>(equipment != nullptr ? equipment->left_extra : 0);
    out["ammo_extra"] = static_cast<std::int64_t>(equipment != nullptr ? equipment->ammo_extra : 0);
    godot::PackedInt64Array worn;
    if (equipment != nullptr) {
        for (const auto form : equipment->worn) {
            worn.push_back(static_cast<std::int64_t>(form));
        }
    }
    out["worn"] = worn;
    return out;
}

Dictionary SkydotPapyrus::ranged_weapon(std::int64_t owner) {
    const auto id = static_cast<std::uint32_t>(owner);
    resolve_owner(id);
    Dictionary out;
    std::vector<std::uint32_t> forms;
    for (const auto& [form, total] : vm_->inventory().items(id)) {
        if (total > 0) {
            forms.push_back(form);
        }
    }
    const auto gear = vm::choose_gear(forms, [this](std::uint32_t form) { return item_info_of(form); });
    if (gear.bow != 0) {
        out["weapon"] = static_cast<std::int64_t>(gear.bow);
        out["ammo"] = static_cast<std::int64_t>(gear.ammo);
    }
    return out;
}

std::int64_t SkydotPapyrus::melee_weapon(std::int64_t owner) {
    const auto id = static_cast<std::uint32_t>(owner);
    resolve_owner(id);
    std::vector<std::uint32_t> forms;
    for (const auto& [form, total] : vm_->inventory().items(id)) {
        if (total > 0) {
            forms.push_back(form);
        }
    }
    return static_cast<std::int64_t>(
        vm::choose_gear(forms, [this](std::uint32_t form) { return item_info_of(form); }).melee);
}

std::int64_t SkydotPapyrus::equipped_item_type(std::int64_t owner, std::int64_t hand) {
    const auto id = static_cast<std::uint32_t>(owner);
    resolve_owner(id);
    const auto* equipment = items_.find(id);
    const std::uint32_t form = equipment == nullptr ? 0 : (hand == 0 ? equipment->left : equipment->right);
    return form == 0 ? 0 : vm::equipped_item_type(item_info_of(form));
}

std::int64_t SkydotPapyrus::drop(std::int64_t owner, std::int64_t form, std::int64_t count, std::int64_t cell,
                                 const godot::Vector3& position, const godot::Vector3& rotation,
                                 std::int64_t extra) {
    const auto base = static_cast<std::uint32_t>(form);
    const auto from = static_cast<std::uint32_t>(owner);
    resolve_owner(from);
    const std::uint32_t stack = resolve_extra(from, base, extra);
    const std::int64_t taken = remove_item_extra(from, base, stack, count);
    if (taken <= 0) {
        return 0;
    }
    vm::PlacedRef placed;
    placed.base = base;
    placed.cell = static_cast<std::uint32_t>(cell);
    for (int i = 0; i < 3; ++i) {
        placed.position[i] = static_cast<double>(position[i]);
        placed.rotation[i] = static_cast<double>(rotation[i]);
    }
    const std::uint32_t id = place_ref(placed, false);
    if (id == 0) {
        vm_->inventory().add(from, base, taken, stack);
        return 0;
    }
    items_.set_dropped(id, taken, stack);
    return id;
}

// ---- natives ----------------------------------------------------------------------

void SkydotPapyrus::bind_item_natives() {
    using natives::arg;
    using natives::result;
    using vm::NativeCall;
    using vm::Value;
    const auto is_object = [](NativeCall& c, std::size_t i) { return arg(c, i).kind() == vm::Kind::object; };

    vm_->bind("Actor", "EquipItem", [this, is_object](NativeCall& c) {
        if (is_object(c, 0)) {
            equip_item(c.self.form(), arg(c, 0).form(), false, true);
        }
        return vm::NativeResult{};
    });
    vm_->bind("Actor", "UnequipItem", [this, is_object](NativeCall& c) {
        if (is_object(c, 0)) {
            unequip_item(c.self.form(), arg(c, 0).form());
        }
        return vm::NativeResult{};
    });
    vm_->bind("Actor", "UnequipAll", [this](NativeCall& c) {
        unequip_all(c.self.form());
        return vm::NativeResult{};
    });
    vm_->bind("Actor", "IsEquipped", [this, is_object](NativeCall& c) {
        resolve_owner(c.self.form());
        const auto* equipment = items_.find(c.self.form());
        return result(Value::boolean(is_object(c, 0) && equipment != nullptr && equipment->holds(arg(c, 0).form())));
    });
    // aiHand: 0 left, 1 right.
    vm_->bind("Actor", "GetEquippedItemType", [this](NativeCall& c) {
        return result(Value::integer(static_cast<std::int32_t>(equipped_item_type(c.self.form(), arg(c, 0).i()))));
    });
    vm_->bind("Actor", "GetEquippedWeapon", [this](NativeCall& c) {
        resolve_owner(c.self.form());
        const auto* equipment = items_.find(c.self.form());
        const std::uint32_t form =
            equipment == nullptr ? 0 : (vm::Vm::truthy(arg(c, 0)) ? equipment->left : equipment->right);
        const auto info = item_info_of(form);
        return form != 0 && info.category == vm::ItemCategory::weapon ? result(c.vm.object(form, "Weapon"))
                                                                     : vm::NativeResult{};
    });
    vm_->bind("Actor", "GetEquippedShield", [this](NativeCall& c) {
        resolve_owner(c.self.form());
        const auto* equipment = items_.find(c.self.form());
        const std::uint32_t form = equipment == nullptr ? 0 : equipment->left;
        return form != 0 && vm::is_shield(item_info_of(form)) ? result(c.vm.object(form, "Armor"))
                                                              : vm::NativeResult{};
    });
}

} // namespace skydot
