// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's items: start inventories resolved on first touch, what
// owners hold and wear, and the natives that equip. The rules are
// state/equipment.hpp (pure), the data data/start_inventory.hpp and
// data/leveled.hpp (pure); this is the glue to the VM's inventory, the
// world's records and Godot.
#include "vm/papyrus.hpp"

#include "data/fb_search.hpp"
#include "data/leveled.hpp"
#include "data/start_inventory.hpp"
#include "vm/papyrus_common.hpp"

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

// ---- the game layer's view ------------------------------------------------------

void SkydotPapyrus::resolve(std::int64_t owner) {
    item_.resolve_owner(static_cast<std::uint32_t>(owner));
}

Dictionary SkydotPapyrus::item_info(std::int64_t form) const {
    const auto info = item_.item_info_of(static_cast<std::uint32_t>(form));
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
    item_.resolve_owner(id);
    const auto* equipment = item_.states().find(id);
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
        auto info = item_.item_info_of(form);
        const auto add_row = [&](std::uint32_t extra, std::int64_t count) {
            std::string name = info.name;
            const char* tier = "";
            if (const auto* e = item_.states().extras().find(extra)) {
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
            const auto charge = magic::weapon_charge(*root, row.form, item_.states().extras().find(row.extra), {}, &created_);
            if (charge.enchantment != 0 && charge.max > 0.0F) {
                entry["charge"] = static_cast<double>(charge.charge);
                entry["charge_max"] = static_cast<double>(charge.max);
            }
        } else if (root != nullptr && row.info.category == vm::ItemCategory::soul_gem) {
            const auto gem = magic::gem_info(*root, {}, row.form, item_.states().extras().find(row.extra));
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
    item_.resolve_owner(source);
    item_.resolve_owner(target);
    std::int64_t moved = 0;
    if (extra < 0) {
        for (const auto& part : item_.remove_item_split(source, base, count)) {
            vm_->inventory().add(target, base, part.count, part.extra);
            moved += part.count;
        }
    } else {
        moved = item_.remove_item_extra(source, base, static_cast<std::uint32_t>(extra), count);
        vm_->inventory().add(target, base, moved, static_cast<std::uint32_t>(extra));
    }
    return moved;
}

bool SkydotPapyrus::equip(std::int64_t owner, std::int64_t form, std::int64_t extra) {
    const auto id = static_cast<std::uint32_t>(owner);
    const auto base = static_cast<std::uint32_t>(form);
    item_.resolve_owner(id);
    return item_.equip_item(id, base, false, false, item_.resolve_extra(id, base, extra));
}

bool SkydotPapyrus::unequip(std::int64_t owner, std::int64_t form, std::int64_t extra) {
    return item_.unequip_extra(static_cast<std::uint32_t>(owner), static_cast<std::uint32_t>(form),
                               extra < 0 ? std::nullopt : std::optional<std::uint32_t>(static_cast<std::uint32_t>(extra)));
}

Dictionary SkydotPapyrus::equipment(std::int64_t owner) {
    const auto id = static_cast<std::uint32_t>(owner);
    item_.resolve_owner(id);
    Dictionary out;
    const auto* equipment = item_.states().find(id);
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
    item_.resolve_owner(id);
    Dictionary out;
    std::vector<std::uint32_t> forms;
    for (const auto& [form, total] : vm_->inventory().items(id)) {
        if (total > 0) {
            forms.push_back(form);
        }
    }
    const auto gear = vm::choose_gear(forms, [this](std::uint32_t form) { return item_.item_info_of(form); });
    if (gear.bow != 0) {
        out["weapon"] = static_cast<std::int64_t>(gear.bow);
        out["ammo"] = static_cast<std::int64_t>(gear.ammo);
    }
    return out;
}

std::int64_t SkydotPapyrus::melee_weapon(std::int64_t owner) {
    const auto id = static_cast<std::uint32_t>(owner);
    item_.resolve_owner(id);
    std::vector<std::uint32_t> forms;
    for (const auto& [form, total] : vm_->inventory().items(id)) {
        if (total > 0) {
            forms.push_back(form);
        }
    }
    return static_cast<std::int64_t>(
        vm::choose_gear(forms, [this](std::uint32_t form) { return item_.item_info_of(form); }).melee);
}

std::int64_t SkydotPapyrus::equipped_item_type(std::int64_t owner, std::int64_t hand) {
    return item_.equipped_item_type(static_cast<std::uint32_t>(owner), hand);
}

std::int64_t SkydotPapyrus::drop(std::int64_t owner, std::int64_t form, std::int64_t count, std::int64_t cell,
                                 const godot::Vector3& position, const godot::Vector3& rotation,
                                 std::int64_t extra) {
    const auto base = static_cast<std::uint32_t>(form);
    const auto from = static_cast<std::uint32_t>(owner);
    item_.resolve_owner(from);
    const std::uint32_t stack = item_.resolve_extra(from, base, extra);
    const std::int64_t taken = item_.remove_item_extra(from, base, stack, count);
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
    item_.states().set_dropped(id, taken, stack);
    return id;
}

} // namespace skydot
