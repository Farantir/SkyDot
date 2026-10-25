// SPDX-License-Identifier: GPL-3.0-or-later
//
// Items: the equip rules (vm/equipment.cpp), the state a save keeps
// (vm/item_states.cpp) and what containers and actors start with
// (data/start_inventory.cpp), on worlds built in memory.
#include "support/world_builder.hpp"

#include "data/actors.hpp"
#include "data/fb_search.hpp"
#include "data/leveled.hpp"
#include "data/start_inventory.hpp"
#include "save/bytes.hpp"
#include "vm/equipment.hpp"
#include "vm/item_states.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <set>

using namespace skydot::testing;
namespace vm = skydot::vm;

namespace {

constexpr std::uint32_t k_lvli = 0x494C564C;
constexpr std::uint32_t k_sword = 0x10;
constexpr std::uint32_t k_dagger = 0x11;
constexpr std::uint32_t k_greatsword = 0x12;
constexpr std::uint32_t k_bow = 0x13;
constexpr std::uint32_t k_shield = 0x14;
constexpr std::uint32_t k_torch = 0x15;
constexpr std::uint32_t k_cuirass = 0x20;
constexpr std::uint32_t k_helmet = 0x21;
constexpr std::uint32_t k_robes = 0x22;
constexpr std::uint32_t k_boots = 0x23;
constexpr std::uint32_t k_arrow = 0x30;
constexpr std::uint32_t k_potion = 0x40;
constexpr std::uint32_t k_book = 0x41;
constexpr std::uint32_t k_gold = 0x42;
constexpr std::uint32_t k_key = 0x43;

vm::ItemInfo item(std::uint32_t form, vm::ItemCategory category) {
    vm::ItemInfo out;
    out.known = true;
    out.form = form;
    out.category = category;
    return out;
}
vm::ItemInfo weapon(std::uint32_t form, std::uint32_t slot, std::uint8_t type) {
    auto out = item(form, vm::ItemCategory::weapon);
    out.equip_slot = slot;
    out.weapon_type = type;
    return out;
}
vm::ItemInfo armor(std::uint32_t form, std::uint32_t slots, vm::ItemCategory category = vm::ItemCategory::armor,
                   std::uint32_t slot = 0) {
    auto out = item(form, category);
    out.equip_slot = slot;
    out.biped_slots = slots;
    return out;
}
vm::ItemInfo other(std::uint32_t form, vm::ItemCategory category) {
    return item(form, category);
}

const std::map<std::uint32_t, vm::ItemInfo>& catalog() {
    static const std::map<std::uint32_t, vm::ItemInfo> items = {
        {k_sword, weapon(k_sword, vm::etyp::either_hand, 1)},
        {k_dagger, weapon(k_dagger, vm::etyp::either_hand, 2)},
        {k_greatsword, weapon(k_greatsword, vm::etyp::both_hands, 5)},
        {k_bow, weapon(k_bow, vm::etyp::both_hands, 7)},
        {k_shield, armor(k_shield, 1U << 9, vm::ItemCategory::armor, vm::etyp::shield)},
        {k_torch, other(k_torch, vm::ItemCategory::light)},
        {k_cuirass, armor(k_cuirass, 1U << 2)},
        {k_helmet, armor(k_helmet, (1U << 0) | (1U << 1))},
        {k_robes, armor(k_robes, (1U << 2) | (1U << 7), vm::ItemCategory::clothing)},
        {k_boots, armor(k_boots, 1U << 7)},
        {k_arrow, other(k_arrow, vm::ItemCategory::ammo)},
        {k_potion, other(k_potion, vm::ItemCategory::potion)},
        {k_book, other(k_book, vm::ItemCategory::book)},
        {k_gold, other(k_gold, vm::ItemCategory::misc)},
        {k_key, other(k_key, vm::ItemCategory::key)},
    };
    return items;
}

vm::ItemInfo info(std::uint32_t form) {
    const auto it = catalog().find(form);
    return it == catalog().end() ? vm::ItemInfo{} : it->second;
}

vm::EquipResult put(vm::Equipment& e, std::uint32_t form, bool left = false) {
    return vm::equip(e, info(form), left, info);
}

} // namespace

TEST_CASE("one-handed weapons go right, or left when asked", "[items][equip]") {
    vm::Equipment e;
    // A weapon made for the right hand can be asked into the left too.
    auto right_only = weapon(0x70, vm::etyp::right_hand, 1);
    CHECK(vm::equip(e, right_only, true, info).action == vm::EquipAction::equipped);
    CHECK(e.left == 0x70);
    CHECK(e.right == 0);
    vm::unequip(e, 0x70);
    CHECK(put(e, k_sword).action == vm::EquipAction::equipped);
    CHECK(e.right == k_sword);
    CHECK(e.left == 0);
    // Either hand: left when asked, the right one stays.
    put(e, k_dagger, true);
    CHECK(e.right == k_sword);
    CHECK(e.left == k_dagger);
    // A new right-hand weapon replaces the old one.
    const auto result = put(e, k_dagger);
    CHECK(e.right == k_dagger);
    CHECK(result.displaced == std::vector<std::uint32_t>{k_sword});
}

TEST_CASE("a two-hander takes both hands and is cleared by a shield or an off-hand weapon", "[items][equip]") {
    vm::Equipment e;
    put(e, k_sword);
    put(e, k_shield);
    CHECK(e.left == k_shield);
    const auto two = put(e, k_greatsword);
    CHECK(e.right == k_greatsword);
    CHECK(e.left == 0);
    CHECK(std::ranges::count(two.displaced, k_shield) == 1);
    CHECK(std::ranges::count(two.displaced, k_sword) == 1);

    // A shield clears the two-hander.
    const auto shield = put(e, k_shield);
    CHECK(e.left == k_shield);
    CHECK(e.right == 0);
    CHECK(shield.displaced == std::vector<std::uint32_t>{k_greatsword});

    // And so does a torch, or a weapon asked for the left.
    put(e, k_bow);
    CHECK(e.right == k_bow);
    put(e, k_torch);
    CHECK(e.left == k_torch);
    CHECK(e.right == 0);
    put(e, k_bow);
    put(e, k_dagger, true);
    CHECK(e.left == k_dagger);
    CHECK(e.right == 0);
}

TEST_CASE("worn armor replaces what overlaps its slots", "[items][equip]") {
    vm::Equipment e;
    put(e, k_cuirass);
    put(e, k_boots);
    put(e, k_helmet);
    CHECK(e.worn == std::set<std::uint32_t>{k_cuirass, k_boots, k_helmet});
    // Robes cover the body and the feet.
    const auto robes = put(e, k_robes);
    CHECK(e.worn == std::set<std::uint32_t>{k_robes, k_helmet});
    CHECK(robes.displaced.size() == 2);
    // The same piece again changes nothing.
    CHECK(put(e, k_robes).displaced.empty());
    CHECK(e.worn == std::set<std::uint32_t>{k_robes, k_helmet});
}

TEST_CASE("ammo has a slot, and items that are used up are not held", "[items][equip]") {
    vm::Equipment e;
    CHECK(put(e, k_arrow).action == vm::EquipAction::equipped);
    CHECK(e.ammo == k_arrow);
    CHECK(put(e, k_potion).action == vm::EquipAction::consumed);
    CHECK(put(e, k_book).action == vm::EquipAction::read);
    CHECK(put(e, k_gold).action == vm::EquipAction::refused);
    CHECK(put(e, k_key).action == vm::EquipAction::refused);
    CHECK(e.holds(k_arrow));
    CHECK_FALSE(e.holds(k_potion));
}

TEST_CASE("unequip takes an item off wherever it is", "[items][equip]") {
    vm::Equipment e;
    put(e, k_sword);
    put(e, k_shield);
    put(e, k_cuirass);
    CHECK(vm::unequip(e, k_shield));
    CHECK(vm::unequip(e, k_cuirass));
    CHECK(vm::unequip(e, k_sword));
    CHECK_FALSE(vm::unequip(e, k_sword));
    CHECK(e.empty());
}

TEST_CASE("a weapon without an ETYP is two-handed by its animation type", "[items][equip]") {
    auto bare = weapon(k_bow, 0, 7);
    CHECK(vm::is_two_handed(bare));
    CHECK_FALSE(vm::is_two_handed(weapon(k_sword, 0, 1)));
    CHECK_FALSE(vm::is_two_handed(armor(k_cuirass, 4)));
}

TEST_CASE("GetEquippedItemType values", "[items][equip]") {
    CHECK(vm::equipped_item_type(info(k_sword)) == 1);
    CHECK(vm::equipped_item_type(info(k_dagger)) == 2);
    CHECK(vm::equipped_item_type(info(k_greatsword)) == 5);
    CHECK(vm::equipped_item_type(info(k_bow)) == 7);
    CHECK(vm::equipped_item_type(info(k_shield)) == 10);
    CHECK(vm::equipped_item_type(info(k_torch)) == 11);
    CHECK(vm::equipped_item_type(weapon(1, 0, 9)) == 12);
    CHECK(vm::equipped_item_type(weapon(1, 0, 0)) == 0);
}

TEST_CASE("item states survive a save, and an empty section loads", "[items][save]") {
    vm::ItemStates s;
    s.mark_resolved(0x800);
    s.mark_resolved(0x801);
    s.set_pick(0x801, 0x1234);
    auto& e = s.equipment(0x801);
    e.right = k_sword;
    e.left = k_shield;
    e.ammo = k_arrow;
    e.worn = {k_cuirass, k_boots};
    s.equipment(0x802); // empty: not saved
    s.set_dropped(0xFF000001, 7);

    vm::ItemStates back;
    REQUIRE(vm::ItemStates::load(skydot::save::ByteReader(s.save()), back));
    CHECK(back.resolved(0x800));
    CHECK(back.resolved(0x801));
    CHECK(back.pick_of(0x801) == 0x1234);
    CHECK(back.pick_of(0x800) == 0);
    REQUIRE(back.find(0x801) != nullptr);
    CHECK(back.find(0x801)->right == k_sword);
    CHECK(back.find(0x801)->left == k_shield);
    CHECK(back.find(0x801)->ammo == k_arrow);
    CHECK(back.find(0x801)->worn == std::set<std::uint32_t>{k_cuirass, k_boots});
    CHECK(back.find(0x802) == nullptr);
    CHECK(back.dropped_count(0xFF000001) == 7);
    CHECK(back.dropped_count(0xFF000002) == 0);

    vm::ItemStates none;
    REQUIRE(vm::ItemStates::load(skydot::save::ByteReader(vm::ItemStates{}.save()), none));
    CHECK_FALSE(none.resolved(0x800));

    // Truncated and trailing bytes are refused.
    auto bytes = s.save();
    bytes.pop_back();
    CHECK_FALSE(vm::ItemStates::load(skydot::save::ByteReader(bytes), none));
    bytes = s.save();
    bytes.push_back(0);
    CHECK_FALSE(vm::ItemStates::load(skydot::save::ByteReader(bytes), none));
}

TEST_CASE("forgetting an owner drops its states", "[items][save]") {
    vm::ItemStates s;
    s.mark_resolved(0x800);
    s.set_pick(0x800, 5);
    s.equipment(0x800).right = k_sword;
    s.erase(0x800);
    CHECK_FALSE(s.resolved(0x800));
    CHECK(s.pick_of(0x800) == 0);
    CHECK(s.find(0x800) == nullptr);
}

// ---- what owners start with -------------------------------------------------------

namespace {

constexpr std::uint32_t k_chest = 0x300;
constexpr std::uint32_t k_loot = 0x310;
constexpr std::uint32_t k_outfit = 0x320;
constexpr std::uint32_t k_npc = 0x330;
constexpr std::uint32_t k_template_npc = 0x331;

WorldSpec stocked() {
    WorldSpec spec;
    spec.bases = {{.id = k_chest, .items = {{k_gold, 25}, {k_loot, 2}, {k_potion, 0}}}};
    spec.leveled_lists = {{.id = k_loot, .type = k_lvli, .entries = {{1, k_sword}, {1, k_dagger}}}};
    return spec;
}

} // namespace

TEST_CASE("a container's contents are its items with its lists drawn per count", "[items][start]") {
    const BuiltWorld world(stocked());
    const auto items = skydot::container_contents(*world, k_chest, 0x800, 1);
    std::int64_t gold = 0, weapons = 0;
    for (const auto& item : items) {
        if (item.form == k_gold) {
            gold = item.count;
        } else {
            CHECK((item.form == k_sword || item.form == k_dagger));
            weapons += item.count;
        }
        CHECK(item.form != k_potion); // a count of 0 is nothing
    }
    CHECK(gold == 25);
    CHECK(weapons == 2); // the list twice, one item each

    // The same for a reference every time, and different references differ.
    CHECK(skydot::container_contents(*world, k_chest, 0x800, 1).size() == items.size());
    std::set<std::uint32_t> firsts;
    for (std::uint32_t ref = 0x800; ref < 0x840; ++ref) {
        for (const auto& item : skydot::container_contents(*world, k_chest, ref, 1)) {
            if (item.form != k_gold) {
                firsts.insert(item.form);
            }
        }
    }
    CHECK(firsts == std::set<std::uint32_t>{k_sword, k_dagger});

    // Something that is no container holds nothing.
    CHECK(skydot::container_contents(*world, 0x999, 0x800, 1).empty());
}

TEST_CASE("an actor starts with its items and its outfit, worn", "[items][start]") {
    auto spec = stocked();
    spec.outfits = {{.id = k_outfit, .items = {k_cuirass, k_loot}}};
    spec.npcs = {{.id = k_npc, .default_outfit = k_outfit, .items = {{k_gold, 3}}}};
    const BuiltWorld world(spec);
    const auto inventory = skydot::actor_contents(*world, k_npc, 0x800);
    // The outfit: the cuirass and one of the list's weapons.
    REQUIRE(inventory.worn.size() == 2);
    CHECK(inventory.worn[0] == k_cuirass);
    CHECK((inventory.worn[1] == k_sword || inventory.worn[1] == k_dagger));
    std::map<std::uint32_t, std::int64_t> counts;
    for (const auto& item : inventory.items) {
        counts[item.form] += item.count;
    }
    CHECK(counts[k_gold] == 3);
    CHECK(counts[k_cuirass] == 1);
    CHECK(counts[inventory.worn[1]] == 1);
}

TEST_CASE("an actor with a template for its inventory takes the template's", "[items][start]") {
    auto spec = stocked();
    spec.npcs = {{.id = k_npc,
                  .template_id = k_template_npc,
                  .template_flags = wfb::NpcTemplateFlags::use_inventory,
                  .items = {{k_dagger, 1}}},
                 {.id = k_template_npc, .items = {{k_gold, 9}}}};
    const BuiltWorld world(spec);
    const auto inventory = skydot::actor_contents(*world, k_npc, 0x800);
    REQUIRE(inventory.items.size() == 1);
    CHECK(inventory.items[0].form == k_gold);
    CHECK(inventory.items[0].count == 9);
}

TEST_CASE("an actor's death item is an item or a leveled list, from the template with the inventory",
          "[items][start]") {
    auto spec = stocked();
    spec.npcs = {{.id = k_npc, .death_item = k_gold},
                 {.id = 0x332, .death_item = k_loot},
                 {.id = 0x333, .template_id = k_template_npc, .template_flags = wfb::NpcTemplateFlags::use_inventory},
                 {.id = k_template_npc, .death_item = k_dagger},
                 {.id = 0x334}};
    const BuiltWorld world(spec);
    const auto plain = skydot::death_contents(*world, k_npc, 0x800);
    REQUIRE(plain.size() == 1);
    CHECK(plain[0].form == k_gold);
    CHECK(plain[0].count == 1);
    const auto drawn = skydot::death_contents(*world, 0x332, 0x800);
    REQUIRE(drawn.size() == 1);
    CHECK((drawn[0].form == k_sword || drawn[0].form == k_dagger));
    // The same reference draws the same.
    CHECK(skydot::death_contents(*world, 0x332, 0x800)[0].form == drawn[0].form);
    const auto inherited = skydot::death_contents(*world, 0x333, 0x800);
    REQUIRE(inherited.size() == 1);
    CHECK(inherited[0].form == k_dagger);
    CHECK(skydot::death_contents(*world, 0x334, 0x800).empty());
}

TEST_CASE("the outfit the VM draws is the one the builder wears", "[items][start]") {
    // Same lists, same seeds: for every reference the VM's worn pieces are
    // what plan_actor puts on the actor.
    constexpr std::uint32_t k_race = 0x100;
    constexpr std::uint32_t k_skin_armor = 0x200;
    WorldSpec spec;
    spec.races = {{.id = k_race,
                   .skeletons = {"meshes/s.nif", "meshes/s.nif"},
                   .behaviours = {"meshes/b.hkx", "meshes/b.hkx"},
                   .skin = k_skin_armor}};
    spec.armors = {{.id = k_helmet, .slots = 3, .addons = {0x401}},
                   {.id = k_boots, .slots = 0x80, .addons = {0x402}}};
    spec.addons = {{.id = 0x401, .slots = 3, .race = k_race, .male_model = "meshes/helmet.nif"},
                   {.id = 0x402, .slots = 0x80, .race = k_race, .male_model = "meshes/boots.nif"}};
    spec.leveled_lists = {{.id = k_loot, .type = k_lvli, .entries = {{1, k_helmet}, {1, k_boots}}}};
    spec.outfits = {{.id = k_outfit, .items = {k_loot}}};
    spec.npcs = {{.id = k_npc, .race = k_race, .default_outfit = k_outfit}};
    const BuiltWorld world(spec);
    const std::set<std::string> pack = {"meshes/s.hkx", "meshes/helmet.nif", "meshes/boots.nif"};
    const auto exists = [&](const std::string& path) { return pack.contains(path); };
    std::set<std::uint32_t> seen;
    for (std::uint32_t ref = 0x800; ref < 0x840; ++ref) {
        const auto vm_worn = skydot::actor_contents(*world, k_npc, ref).worn;
        REQUIRE(vm_worn.size() == 1);
        seen.insert(vm_worn[0]);
        const auto plan = skydot::plan_actor(*world, k_npc, ref, exists);
        const std::string model = vm_worn[0] == k_helmet ? "meshes/helmet.nif" : "meshes/boots.nif";
        CHECK(std::ranges::count(plan.parts, model) == 1);
        // What the VM says the actor wears overrides the outfit.
        const auto dressed = skydot::plan_actor(*world, k_npc, ref, exists,
                                                [&](std::uint32_t, std::vector<std::uint32_t>& armors) {
                                                    armors = {vm_worn[0] == k_helmet ? k_boots : k_helmet};
                                                    return true;
                                                });
        CHECK(std::ranges::count(dressed.parts, model) == 0);
        const auto naked = skydot::plan_actor(*world, k_npc, ref, exists,
                                              [](std::uint32_t, std::vector<std::uint32_t>&) { return true; });
        CHECK(std::ranges::count(naked.parts, "meshes/helmet.nif") == 0);
        CHECK(std::ranges::count(naked.parts, "meshes/boots.nif") == 0);
    }
    CHECK(seen == std::set<std::uint32_t>{k_helmet, k_boots});
}

TEST_CASE("an actor's level: its own, or the player's times its multiplier", "[items][start]") {
    WorldSpec spec;
    spec.npcs = {{.id = 1, .level = 7},
                 {.id = 2, .level = 0},
                 {.id = 3, .flags = wfb::NpcFlags::pc_level_mult, .level = 2000}};
    const BuiltWorld world(spec);
    const auto* seven = skydot::lookup(world->npcs(), 1);
    const auto* zero = skydot::lookup(world->npcs(), 2);
    const auto* mult = skydot::lookup(world->npcs(), 3);
    REQUIRE((seven != nullptr && zero != nullptr && mult != nullptr));
    CHECK(skydot::actor_level(*seven) == 7);
    CHECK(skydot::actor_level(*zero) == 1);
    CHECK(skydot::actor_level(*mult) == 2); // 1 x 2.0
}

TEST_CASE("owners that hold items count as resolved when a save has no ITST", "[items][save]") {
    vm::Inventory inventory;
    inventory.add(0x800, k_gold, 5);
    inventory.add(0x801, k_sword, 1);
    vm::ItemStates s;
    s.mark_holders_resolved(inventory);
    CHECK(s.resolved(0x800));
    CHECK(s.resolved(0x801));
    CHECK_FALSE(s.resolved(0x802));
}

TEST_CASE("an ITST of version 1 has no dropped references", "[items][save]") {
    // Version 1: resolved owners, picks and equipment only.
    skydot::save::ByteWriter w;
    w.u32(1);
    w.u32(0x800);
    w.u32(0);
    w.u32(0);
    vm::ItemStates back;
    REQUIRE(vm::ItemStates::load(skydot::save::ByteReader(w.out), back, 1));
    CHECK(back.resolved(0x800));
    CHECK_FALSE(vm::ItemStates::load(skydot::save::ByteReader(w.out), back, 2));
}

TEST_CASE("an NPC fights with its best weapon, a shield beside a one-hander, and keeps its bow", "[items][equip]") {
    const auto with_damage = [](vm::ItemInfo base, float damage, float rating = 0.0F) {
        base.damage = damage;
        base.armor_rating = rating;
        return base;
    };
    const std::map<std::uint32_t, vm::ItemInfo> things = {
        {0x1, with_damage(weapon(0x1, vm::etyp::either_hand, 1), 7)},
        {0x2, with_damage(weapon(0x2, vm::etyp::either_hand, 3), 8)},
        {0x3, with_damage(weapon(0x3, vm::etyp::both_hands, 5), 20)},
        {0x4, with_damage(weapon(0x4, vm::etyp::both_hands, 7), 9)},
        {0x5, with_damage(weapon(0x5, vm::etyp::both_hands, 7), 12)},
        {0x6, with_damage(armor(0x6, 1U << 9, vm::ItemCategory::armor, vm::etyp::shield), 0, 20)},
        {0x7, with_damage(armor(0x7, 1U << 9, vm::ItemCategory::armor, vm::etyp::shield), 0, 30)},
        {0x8, with_damage(other(0x8, vm::ItemCategory::ammo), 8)},
        {0x9, with_damage(other(0x9, vm::ItemCategory::ammo), 10)},
        {0xA, with_damage(weapon(0xA, 0, 8), 30)}, // a staff is no melee weapon
    };
    const vm::ItemLookup lookup = [&](std::uint32_t form) {
        const auto it = things.find(form);
        return it == things.end() ? vm::ItemInfo{} : it->second;
    };
    // One-handers: the axe (8) beats the sword (7); the better shield stands beside it.
    auto gear = vm::choose_gear({0x1, 0x2, 0x6, 0x7, 0x4, 0x5, 0x8, 0x9, 0xA, 0x99}, lookup);
    CHECK(gear.melee == 0x2);
    CHECK(gear.shield == 0x7);
    CHECK(gear.bow == 0x5);
    CHECK(gear.ammo == 0x9);
    // A two-hander of more damage takes both hands: no shield.
    gear = vm::choose_gear({0x1, 0x3, 0x6}, lookup);
    CHECK(gear.melee == 0x3);
    CHECK(gear.shield == 0);
    CHECK(gear.bow == 0);
    // Only a bow: nothing in hand, the bow is the ranged choice.
    gear = vm::choose_gear({0x4, 0x8}, lookup);
    CHECK(gear.melee == 0);
    CHECK(gear.bow == 0x4);
    CHECK(gear.ammo == 0x8);
    CHECK(vm::choose_gear({}, lookup).melee == 0);
}
