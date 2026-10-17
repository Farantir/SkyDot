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
        return;
    }
    for (const auto& item : container_contents(*root, base, owner, leveled::player_level(), global)) {
        inventory.add(owner, item.form, item.count);
    }
}

std::int64_t SkydotPapyrus::remove_item(std::uint32_t owner, std::uint32_t form, std::int64_t count) {
    resolve_owner(owner);
    const std::int64_t taken = vm_->inventory().remove(owner, form, count);
    if (taken > 0 && vm_->inventory().count(owner, form) == 0 && items_.find(owner) != nullptr) {
        vm::unequip(items_.equipment(owner), form);
    }
    return taken;
}

bool SkydotPapyrus::equip_item(std::uint32_t owner, std::uint32_t form, bool left, bool add_missing) {
    resolve_owner(owner);
    const auto info = item_info_of(form);
    if (!info.known) {
        return false;
    }
    const bool carried = vm_->inventory().count(owner, form) > 0;
    if (!carried && !add_missing) {
        return false;
    }
    const auto lookup_info = [this](std::uint32_t id) { return item_info_of(id); };
    vm::Equipment trial = items_.find(owner) != nullptr ? *items_.find(owner) : vm::Equipment{};
    const auto result = vm::equip(trial, info, left, lookup_info);
    if (result.action == vm::EquipAction::refused) {
        return false;
    }
    if (!carried) {
        vm_->inventory().add(owner, form, 1);
    }
    switch (result.action) {
    case vm::EquipAction::equipped:
        items_.equipment(owner) = std::move(trial);
        break;
    case vm::EquipAction::consumed:
        remove_item(owner, form, 1);
        emit_signal("trace", to_godot("item " + info.name + ": effects not implemented"));
        godot::UtilityFunctions::print(to_godot("items: " + info.name + " consumed, effects not implemented"));
        break;
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
    return items_.find(owner) != nullptr && vm::unequip(items_.equipment(owner), form);
}

void SkydotPapyrus::unequip_all(std::uint32_t owner) {
    resolve_owner(owner);
    items_.clear_equipment(owner);
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
    return out;
}

Array SkydotPapyrus::inventory(std::int64_t owner) {
    const auto id = static_cast<std::uint32_t>(owner);
    resolve_owner(id);
    const auto* equipment = items_.find(id);
    struct Row {
        std::string key;
        std::uint32_t form;
        std::int64_t count;
        vm::ItemInfo info;
    };
    std::vector<Row> rows;
    for (const auto& [form, count] : vm_->inventory().items(id)) {
        auto info = item_info_of(form);
        rows.push_back({lower(info.name), form, count, std::move(info)});
    }
    std::ranges::sort(rows, [](const Row& a, const Row& b) {
        return a.key != b.key ? a.key < b.key : a.form < b.form;
    });
    Array out;
    for (const auto& row : rows) {
        Dictionary entry;
        entry["form"] = static_cast<std::int64_t>(row.form);
        entry["name"] = to_godot(row.info.name);
        entry["count"] = row.count;
        entry["weight"] = static_cast<double>(row.info.weight);
        entry["value"] = static_cast<std::int64_t>(row.info.value);
        entry["category"] = vm::category_name(row.info.category);
        entry["equipped"] = equipment != nullptr && equipment->holds(row.form);
        entry["playable"] = row.info.playable;
        out.push_back(entry);
    }
    return out;
}

std::int64_t SkydotPapyrus::transfer(std::int64_t from, std::int64_t to, std::int64_t form, std::int64_t count) {
    const auto source = static_cast<std::uint32_t>(from);
    const auto target = static_cast<std::uint32_t>(to);
    resolve_owner(target);
    const std::int64_t moved = remove_item(source, static_cast<std::uint32_t>(form), count);
    if (moved > 0) {
        vm_->inventory().add(target, static_cast<std::uint32_t>(form), moved);
    }
    return moved;
}

bool SkydotPapyrus::equip(std::int64_t owner, std::int64_t form) {
    return equip_item(static_cast<std::uint32_t>(owner), static_cast<std::uint32_t>(form), false, false);
}

bool SkydotPapyrus::unequip(std::int64_t owner, std::int64_t form) {
    return unequip_item(static_cast<std::uint32_t>(owner), static_cast<std::uint32_t>(form));
}

Dictionary SkydotPapyrus::equipment(std::int64_t owner) {
    const auto id = static_cast<std::uint32_t>(owner);
    resolve_owner(id);
    Dictionary out;
    const auto* equipment = items_.find(id);
    out["right"] = static_cast<std::int64_t>(equipment != nullptr ? equipment->right : 0);
    out["left"] = static_cast<std::int64_t>(equipment != nullptr ? equipment->left : 0);
    out["ammo"] = static_cast<std::int64_t>(equipment != nullptr ? equipment->ammo : 0);
    godot::PackedInt64Array worn;
    if (equipment != nullptr) {
        for (const auto form : equipment->worn) {
            worn.push_back(static_cast<std::int64_t>(form));
        }
    }
    out["worn"] = worn;
    return out;
}

std::int64_t SkydotPapyrus::drop(std::int64_t owner, std::int64_t form, std::int64_t count, std::int64_t cell,
                                 const godot::Vector3& position, const godot::Vector3& rotation) {
    const auto base = static_cast<std::uint32_t>(form);
    const std::int64_t taken = remove_item(static_cast<std::uint32_t>(owner), base, count);
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
        vm_->inventory().add(static_cast<std::uint32_t>(owner), base, taken);
        return 0;
    }
    items_.set_dropped(id, taken);
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
        resolve_owner(c.self.form());
        const auto* equipment = items_.find(c.self.form());
        std::uint32_t form = 0;
        if (equipment != nullptr) {
            form = arg(c, 0).i() == 0 ? equipment->left : equipment->right;
        }
        return result(Value::integer(form == 0 ? 0 : vm::equipped_item_type(item_info_of(form))));
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
