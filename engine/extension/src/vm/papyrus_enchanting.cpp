// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's enchanting table: the glue between magic/enchanting.hpp (the rules), the VM's
// inventory and the spell book, and Godot (docs/adr/0008). The menus ask for rows and previews, the
// actions move units and keep the equipment slots that held a changed unit.
#include "vm/papyrus.hpp"

#include "data/fb_search.hpp"

#include <algorithm>
#include <cmath>

using godot::Array;
using godot::Dictionary;
using godot::String;

namespace skydot {

namespace {

namespace wfb = bethconv::pack::wfb;

String to_godot(const std::string& s) {
    return String::utf8(s.c_str(), static_cast<int>(s.size()));
}

std::string utf8(const String& s) {
    const auto bytes = s.utf8();
    return std::string(bytes.get_data(), static_cast<std::size_t>(bytes.length()));
}

const char* refusal_text(magic::Refusal r) {
    switch (r) {
    case magic::Refusal::none:
        return "";
    case magic::Refusal::no_item:
        return "You do not have that item";
    case magic::Refusal::no_enchantment:
        return "It has no enchantment";
    case magic::Refusal::created:
        return "Only a vanilla enchantment can be learned";
    case magic::Refusal::staff:
        return "A staff cannot be enchanted or disenchanted";
    case magic::Refusal::forbidden:
        return "This cannot be enchanted or disenchanted";
    case magic::Refusal::known:
        return "You know this enchantment already";
    case magic::Refusal::enchanted:
        return "It is enchanted already";
    case magic::Refusal::not_enchantable:
        return "This cannot be enchanted";
    case magic::Refusal::unknown_enchantment:
        return "You have not learned this enchantment";
    case magic::Refusal::does_not_fit:
        return "This enchantment does not fit the item";
    case magic::Refusal::no_gem:
        return "It needs a filled soul gem";
    case magic::Refusal::full:
        return "It is fully charged";
    case magic::Refusal::no_effects:
        return "Nothing would be enchanted";
    }
    return "";
}

const char* soul_name(std::uint8_t soul) {
    constexpr const char* names[] = {"", "Petty", "Lesser", "Common", "Greater", "Grand"};
    return soul <= 5 ? names[soul] : "";
}

const char* kind_name(magic::ItemKind kind) {
    return kind == magic::ItemKind::weapon ? "weapon" : (kind == magic::ItemKind::apparel ? "apparel" : "");
}

} // namespace

std::optional<magic::Enchanting> SkydotPapyrus::enchanting_table() {
    auto* book = spell_book();
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    if (book == nullptr || root == nullptr || !vm_) {
        return std::nullopt;
    }
    magic::EnchantKeywords keywords;
    keywords.disallow_enchanting = world_->data().keyword_by_editor_id("MagicDisallowEnchanting");
    keywords.reusable_soul_gem = world_->data().keyword_by_editor_id("ReusableSoulGem");
    return magic::Enchanting(*root, vm_->inventory(), items_.extras(), created_, *book, keywords, k_player_ref,
                             crafting::k_placeholder_skill);
}

std::string SkydotPapyrus::unit_name(std::uint32_t form, std::uint32_t extra) const {
    if (const auto* e = items_.extras().find(extra); e != nullptr && !e->name.empty()) {
        return e->name;
    }
    return item_name_of(form);
}

namespace {

/// The effects as the menus show them: "Fire Damage 10 points" (the MGEF's description filled in).
Array effect_lines(crafting::Crafting* logic, const std::vector<vm::CreatedEffect>& effects) {
    Array out;
    for (const auto& e : effects) {
        out.push_back(to_godot(logic->describe(e.effect, e.magnitude, e.duration)));
    }
    return out;
}

} // namespace

Dictionary SkydotPapyrus::enchant_rows() {
    Dictionary out;
    Array disenchant, items, enchantments, gems;
    out["disenchant"] = disenchant;
    out["items"] = items;
    out["enchantments"] = enchantments;
    out["gems"] = gems;
    auto table = enchanting_table();
    auto* logic = crafting_logic();
    auto* book = spell_book();
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    if (!table || logic == nullptr || book == nullptr || root == nullptr) {
        return out;
    }
    resolve_owner(k_player_ref);
    magic::EnchantKeywords kw;
    kw.disallow_enchanting = world_->data().keyword_by_editor_id("MagicDisallowEnchanting");
    kw.reusable_soul_gem = world_->data().keyword_by_editor_id("ReusableSoulGem");
    const auto& inventory = vm_->inventory();
    for (const auto& [form, total] : inventory.items(k_player_ref)) {
        std::vector<std::uint32_t> stacks;
        if (inventory.count_extra(k_player_ref, form, 0) > 0) {
            stacks.push_back(0);
        }
        for (const auto& [extra, n] : inventory.extras_of(k_player_ref, form)) {
            stacks.push_back(extra);
        }
        const auto kind = magic::item_kind(*root, form);
        for (const std::uint32_t extra : stacks) {
            const auto* e = items_.extras().find(extra);
            const std::int64_t count = inventory.count_extra(k_player_ref, form, extra);
            Dictionary row;
            row["form"] = static_cast<std::int64_t>(form);
            row["extra"] = static_cast<std::int64_t>(extra);
            row["name"] = to_godot(unit_name(form, extra));
            row["count"] = count;
            if (kind != magic::ItemKind::none) {
                const std::uint32_t ench = magic::enchantment_of(*root, form, e);
                if (ench != 0) {
                    const auto check = magic::check_disenchant(*root, kw, book->known_enchantments(), form, e);
                    const auto resolved = magic::resolve_enchantment(*root, &created_, ench);
                    row["enchantment"] = to_godot(resolved ? resolved->name : std::string{});
                    row["ok"] = check.refusal == magic::Refusal::none;
                    row["refusal"] = refusal_text(check.refusal);
                    disenchant.push_back(row);
                } else if (kind != magic::ItemKind::staff) {
                    row["kind"] = kind_name(kind);
                    items.push_back(row);
                }
                continue;
            }
            const auto gem = magic::gem_info(*root, kw, form, e);
            if (gem.gem && gem.soul > 0) {
                row["soul"] = static_cast<std::int64_t>(gem.soul);
                row["soul_name"] = soul_name(gem.soul);
                row["charge"] = magic::soul_charge(gem.soul);
                row["capacity"] = static_cast<std::int64_t>(gem.capacity);
                gems.push_back(row);
            }
        }
    }
    for (const std::uint32_t id : book->known_enchantments()) {
        const auto* ench = lookup(root->enchantments(), id);
        if (ench == nullptr || ench->enchantment_type() != magic::k_ench_type_item ||
            (ench->casting_type() != magic::k_ench_casting_constant &&
             ench->casting_type() != magic::k_ench_casting_fire_and_forget)) {
            continue;
        }
        Dictionary row;
        row["id"] = static_cast<std::int64_t>(id);
        std::string name = ench->name() != nullptr ? ench->name()->str() : std::string{};
        std::vector<vm::CreatedEffect> effects;
        for (const auto& in : magic::inputs_of(ench->effects())) {
            effects.push_back({in.effect, in.magnitude, in.duration, in.area});
            if (name.empty()) {
                name = logic->effect_name(in.effect);
            }
        }
        row["name"] = to_godot(name);
        row["kind"] = ench->casting_type() == magic::k_ench_casting_fire_and_forget ? "weapon" : "apparel";
        row["effects"] = effect_lines(logic, effects);
        enchantments.push_back(row);
    }
    return out;
}

bool SkydotPapyrus::enchantment_fits(std::int64_t item, std::int64_t enchantment) const {
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    const auto* ench = root != nullptr ? lookup(root->enchantments(), static_cast<std::uint32_t>(enchantment)) : nullptr;
    return ench != nullptr && magic::enchantment_fits(*root, *ench, static_cast<std::uint32_t>(item));
}

namespace {

magic::EnchantRequest request_of(std::int64_t item, std::int64_t item_extra, std::int64_t enchantment,
                                 std::int64_t gem, std::int64_t gem_extra, const String& name) {
    return {.item = static_cast<std::uint32_t>(item),
            .item_extra = static_cast<std::uint32_t>(item_extra),
            .enchantment = static_cast<std::uint32_t>(enchantment),
            .gem = static_cast<std::uint32_t>(gem),
            .gem_extra = static_cast<std::uint32_t>(gem_extra),
            .name = utf8(name)};
}

} // namespace

Dictionary SkydotPapyrus::enchant_preview(std::int64_t item, std::int64_t item_extra, std::int64_t enchantment,
                                          std::int64_t gem, std::int64_t gem_extra, const String& name) {
    Dictionary out;
    out["ok"] = false;
    out["refusal"] = String();
    out["name"] = String();
    out["effects"] = Array();
    out["charge"] = 0.0;
    out["cost"] = 0.0;
    out["uses"] = 0;
    out["constant"] = false;
    auto table = enchanting_table();
    auto* logic = crafting_logic();
    if (!table || logic == nullptr) {
        return out;
    }
    resolve_owner(k_player_ref);
    const auto result = table->preview(request_of(item, item_extra, enchantment, gem, gem_extra, name));
    out["ok"] = result.ok();
    out["refusal"] = refusal_text(result.refusal);
    out["name"] = to_godot(result.name);
    out["effects"] = effect_lines(logic, result.effects);
    out["charge"] = static_cast<double>(result.charge_max);
    out["cost"] = result.cost;
    out["uses"] = result.cost > 0.0 ? static_cast<std::int64_t>(std::floor(static_cast<double>(result.charge_max) / result.cost)) : 0;
    out["constant"] = result.constant;
    return out;
}

Dictionary SkydotPapyrus::enchant(std::int64_t item, std::int64_t item_extra, std::int64_t enchantment,
                                  std::int64_t gem, std::int64_t gem_extra, const String& name) {
    Dictionary out;
    out["ok"] = false;
    out["refusal"] = String();
    out["name"] = String();
    out["extra"] = 0;
    out["created"] = 0;
    auto table = enchanting_table();
    if (!table) {
        return out;
    }
    resolve_owner(k_player_ref);
    const auto base = static_cast<std::uint32_t>(item);
    const auto old_extra = static_cast<std::uint32_t>(item_extra);
    const bool equipped = items_.find(k_player_ref) != nullptr && items_.find(k_player_ref)->holds(base, old_extra);
    const auto result = table->enchant(request_of(item, item_extra, enchantment, gem, gem_extra, name));
    out["refusal"] = refusal_text(result.refusal);
    if (!result.ok()) {
        return out;
    }
    // The enchanted unit is the one that was held or worn.
    if (equipped && vm_->inventory().count_extra(k_player_ref, base, old_extra) <= 0) {
        retarget_equipment(k_player_ref, base, old_extra, result.new_extra);
    }
    refresh_equipment(k_player_ref, static_cast<std::uint32_t>(gem));
    sync_worn_magic(k_player_ref);
    out["ok"] = true;
    out["name"] = to_godot(result.name);
    out["extra"] = static_cast<std::int64_t>(result.new_extra);
    out["created"] = static_cast<std::int64_t>(result.created);
    return out;
}

Dictionary SkydotPapyrus::disenchant(std::int64_t form_in, std::int64_t extra_in) {
    Dictionary out;
    out["ok"] = false;
    out["refusal"] = String();
    out["learned"] = String();
    auto table = enchanting_table();
    auto* logic = crafting_logic();
    if (!table || logic == nullptr) {
        return out;
    }
    resolve_owner(k_player_ref);
    const auto form = static_cast<std::uint32_t>(form_in);
    const auto extra = static_cast<std::uint32_t>(extra_in);
    // What it teaches, named before the unit is gone.
    std::string learned;
    if (const auto* root = world_->data().root()) {
        const std::uint32_t id = magic::enchantment_of(*root, form, items_.extras().find(extra));
        if (const auto* ench = lookup(root->enchantments(), id)) {
            const std::uint32_t base = ench->base_enchantment() != 0 ? ench->base_enchantment() : id;
            if (const auto* b = lookup(root->enchantments(), base); b != nullptr && b->name() != nullptr) {
                learned = b->name()->str();
            }
        }
    }
    const auto refusal = table->disenchant(form, extra);
    out["refusal"] = refusal_text(refusal);
    if (refusal != magic::Refusal::none) {
        return out;
    }
    refresh_equipment(k_player_ref, form);
    out["ok"] = true;
    out["learned"] = to_godot(learned);
    return out;
}

Array SkydotPapyrus::recharge_options(std::int64_t item_in, std::int64_t extra_in) {
    Array out;
    auto table = enchanting_table();
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    if (!table || root == nullptr) {
        return out;
    }
    resolve_owner(k_player_ref);
    const auto item = static_cast<std::uint32_t>(item_in);
    const auto extra = static_cast<std::uint32_t>(extra_in);
    const auto info = magic::weapon_charge(*root, item, items_.extras().find(extra), {}, &created_);
    if (info.enchantment == 0 || info.charge >= info.max) {
        return out;
    }
    magic::EnchantKeywords kw;
    kw.reusable_soul_gem = world_->data().keyword_by_editor_id("ReusableSoulGem");
    const auto& inventory = vm_->inventory();
    for (const auto& [form, total] : inventory.items(k_player_ref)) {
        std::vector<std::uint32_t> stacks;
        if (inventory.count_extra(k_player_ref, form, 0) > 0) {
            stacks.push_back(0);
        }
        for (const auto& [e, n] : inventory.extras_of(k_player_ref, form)) {
            stacks.push_back(e);
        }
        for (const std::uint32_t gem_extra : stacks) {
            const auto gem = magic::gem_info(*root, kw, form, items_.extras().find(gem_extra));
            if (!gem.gem || gem.soul == 0) {
                continue;
            }
            Dictionary row;
            row["form"] = static_cast<std::int64_t>(form);
            row["extra"] = static_cast<std::int64_t>(gem_extra);
            row["name"] = to_godot(unit_name(form, gem_extra));
            row["soul"] = static_cast<std::int64_t>(gem.soul);
            row["soul_name"] = soul_name(gem.soul);
            row["charge"] = magic::soul_charge(gem.soul);
            row["fills"] = static_cast<double>(info.charge) + magic::soul_charge(gem.soul) >= static_cast<double>(info.max);
            out.push_back(row);
        }
    }
    return out;
}

Dictionary SkydotPapyrus::recharge(std::int64_t item, std::int64_t extra, std::int64_t gem, std::int64_t gem_extra) {
    Dictionary out;
    out["ok"] = false;
    out["refusal"] = String();
    out["charge"] = 0.0;
    out["max"] = 0.0;
    out["extra"] = static_cast<std::int64_t>(extra);
    auto table = enchanting_table();
    if (!table) {
        return out;
    }
    resolve_owner(k_player_ref);
    const auto base = static_cast<std::uint32_t>(item);
    const auto old_extra = static_cast<std::uint32_t>(extra);
    const bool equipped = items_.find(k_player_ref) != nullptr && items_.find(k_player_ref)->holds(base, old_extra);
    const auto result = table->recharge(base, old_extra, static_cast<std::uint32_t>(gem), static_cast<std::uint32_t>(gem_extra));
    out["refusal"] = refusal_text(result.refusal);
    if (!result.ok()) {
        return out;
    }
    if (equipped && vm_->inventory().count_extra(k_player_ref, base, old_extra) <= 0) {
        retarget_equipment(k_player_ref, base, old_extra, result.new_extra);
    }
    refresh_equipment(k_player_ref, static_cast<std::uint32_t>(gem));
    out["ok"] = true;
    out["charge"] = static_cast<double>(result.charge);
    out["max"] = static_cast<double>(result.max);
    out["extra"] = static_cast<std::int64_t>(result.new_extra);
    return out;
}

Dictionary SkydotPapyrus::fill_gem(std::int64_t actor) {
    Dictionary out;
    out["filled"] = false;
    out["gem"] = 0;
    out["form"] = 0;
    out["extra"] = 0;
    out["soul"] = 0;
    out["name"] = String();
    auto table = enchanting_table();
    if (!table) {
        return out;
    }
    resolve_owner(k_player_ref);
    const auto fill = table->fill_gem(soul_of(static_cast<std::uint32_t>(actor)));
    if (!fill.filled) {
        return out;
    }
    out["filled"] = true;
    out["gem"] = static_cast<std::int64_t>(fill.gem);
    out["form"] = static_cast<std::int64_t>(fill.form);
    out["extra"] = static_cast<std::int64_t>(fill.extra);
    out["soul"] = static_cast<std::int64_t>(fill.soul);
    out["name"] = to_godot(unit_name(fill.form, fill.extra));
    return out;
}

} // namespace skydot
