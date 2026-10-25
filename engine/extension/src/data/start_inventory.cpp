// SPDX-License-Identifier: GPL-3.0-or-later
#include "data/start_inventory.hpp"

#include "data/actors.hpp"
#include "data/fb_search.hpp"

namespace skydot {

namespace wfb = bethconv::pack::wfb;

namespace {

void add(std::vector<StartItem>& out, std::uint32_t form, std::int64_t count) {
    if (form == 0 || count <= 0) {
        return;
    }
    for (auto& item : out) {
        if (item.form == form) {
            item.count += count;
            return;
        }
    }
    out.push_back({form, count});
}

/// The entries of an inventory (CNTO) into `out`.
void add_entries(const wfb::World& w, const flatbuffers::Vector<const wfb::NpcItem*>* entries,
                 std::uint16_t level, leveled::Rng& rng, const leveled::GlobalValue& global,
                 std::vector<StartItem>& out) {
    if (entries == nullptr) {
        return;
    }
    for (const auto* entry : *entries) {
        if (!leveled::is_list(w, entry->form())) {
            add(out, entry->form(), entry->count());
            continue;
        }
        // N of a list is N evaluations.
        for (std::int32_t i = 0; i < entry->count(); ++i) {
            for (const auto& item : leveled::resolve(w, entry->form(), level, rng, global)) {
                add(out, item.form, item.count);
            }
        }
    }
}

} // namespace

std::vector<StartItem> container_contents(const wfb::World& world, std::uint32_t base, std::uint32_t owner,
                                          std::uint16_t level, const leveled::GlobalValue& global) {
    std::vector<StartItem> out;
    const auto* b = lookup(world.bases(), base);
    if (b == nullptr) {
        return out;
    }
    leveled::Rng rng(leveled::seed_for(owner, leveled::k_purpose_inventory));
    add_entries(world, b->items(), level, rng, global, out);
    return out;
}

StartInventory actor_contents(const wfb::World& world, std::uint32_t npc, std::uint32_t owner,
                              const leveled::GlobalValue& global) {
    StartInventory out;
    const auto* base = lookup(world.npcs(), npc);
    if (base == nullptr) {
        return out;
    }
    const auto* inventory = resolve_npc(world, npc, wfb::NpcTemplateFlags::use_inventory, owner);
    if (inventory == nullptr) {
        return out;
    }
    const std::uint16_t level = actor_level(*base);
    leveled::Rng rng(leveled::seed_for(owner, leveled::k_purpose_inventory));
    add_entries(world, inventory->items(), level, rng, global, out.items);

    // The default outfit, drawn as the actor builder draws it.
    const auto* outfit = lookup(world.outfits(), inventory->default_outfit());
    const auto* outfit_items = outfit != nullptr ? outfit->items() : nullptr;
    if (outfit_items != nullptr) {
        leveled::Rng outfit_rng(leveled::seed_for(owner, leveled::k_purpose_outfit));
        for (const std::uint32_t form : *outfit_items) {
            if (!leveled::is_list(world, form)) {
                out.worn.push_back(form);
                continue;
            }
            for (const auto& item : leveled::resolve(world, form, level, outfit_rng, global)) {
                out.worn.push_back(item.form);
            }
        }
        for (const std::uint32_t form : out.worn) {
            add(out.items, form, 1);
        }
    }
    return out;
}

std::vector<StartItem> death_contents(const wfb::World& world, std::uint32_t npc, std::uint32_t owner,
                                      const leveled::GlobalValue& global) {
    std::vector<StartItem> out;
    const auto* base = lookup(world.npcs(), npc);
    if (base == nullptr) {
        return out;
    }
    // The CK shows the death item on the Inventory tab: "Use Inventory" covers it.
    const auto* source = resolve_npc(world, npc, wfb::NpcTemplateFlags::use_inventory, owner);
    if (source == nullptr || source->death_item() == 0) {
        return out;
    }
    const std::uint32_t form = source->death_item();
    if (!leveled::is_list(world, form)) {
        add(out, form, 1);
        return out;
    }
    leveled::Rng rng(leveled::seed_for(owner, leveled::k_purpose_death_item));
    for (const auto& item : leveled::resolve(world, form, actor_level(*base), rng, global)) {
        add(out, item.form, item.count);
    }
    return out;
}

} // namespace skydot
