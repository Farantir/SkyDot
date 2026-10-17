// SPDX-License-Identifier: GPL-3.0-or-later
//
// Item instances and created forms (docs/adr/0007), harvesting and crafting:
// the inventory's splits, ITST version 3 and CRFT, the harvest rules, and the
// crafting rules on a world built in memory.
#include "support/world_builder.hpp"

#include "save/bytes.hpp"
#include "vm/crafting.hpp"
#include "vm/created_forms.hpp"
#include "vm/equipment.hpp"
#include "vm/harvest.hpp"
#include "vm/item_extras.hpp"
#include "vm/item_states.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <set>

using namespace skydot::testing;
namespace vm = skydot::vm;
namespace crafting = skydot::crafting;
using Catch::Approx;

namespace {

constexpr std::uint32_t k_player = 0x14;
constexpr std::uint32_t k_chest = 0x200;
constexpr std::uint32_t k_dagger = 0x13;

vm::ItemExtra fine() {
    vm::ItemExtra e;
    e.health = 1.1F;
    return e;
}

} // namespace

// ---- inventory --------------------------------------------------------------------

TEST_CASE("units with extras are split off the total and removed after the plain ones", "[items][extras]") {
    vm::ItemExtras table;
    const auto fine_id = table.intern(fine());
    vm::ItemExtra better = fine();
    better.health = 1.2F;
    const auto better_id = table.intern(better);
    CHECK(fine_id == 1);
    CHECK(better_id == 2);
    CHECK(table.intern(fine()) == fine_id); // equal extras share an id
    CHECK(table.intern(vm::ItemExtra{}) == 0);
    CHECK(std::string(vm::temper_tier_name(1.1F)) == "Fine");
    CHECK(std::string(vm::temper_tier_name(1.0F)).empty());

    vm::Inventory inv;
    inv.add(k_player, k_dagger, 2);
    inv.add(k_player, k_dagger, 1, better_id);
    inv.add(k_player, k_dagger, 2, fine_id);
    CHECK(inv.count(k_player, k_dagger) == 5); // scripts see the total
    CHECK(inv.count_extra(k_player, k_dagger, 0) == 2);
    CHECK(inv.count_extra(k_player, k_dagger, fine_id) == 2);

    // Plain first, then the extras in id order.
    const auto taken = inv.remove_split(k_player, k_dagger, 4);
    REQUIRE(taken.size() == 2);
    CHECK((taken[0].extra == 0 && taken[0].count == 2));
    CHECK((taken[1].extra == fine_id && taken[1].count == 2));
    CHECK(inv.count(k_player, k_dagger) == 1);
    CHECK(inv.count_extra(k_player, k_dagger, better_id) == 1);
    CHECK(inv.remove(k_player, k_dagger, 5) == 1);
    CHECK(inv.items(k_player).empty());
    CHECK(inv.splits().empty());
}

TEST_CASE("remove_extra takes a named stack, and a transfer keeps the extra", "[items][extras]") {
    vm::Inventory inv;
    inv.add(k_player, k_dagger, 1);
    inv.add(k_player, k_dagger, 2, 1);
    CHECK(inv.remove_extra(k_player, k_dagger, 1, 5) == 2);
    CHECK(inv.count(k_player, k_dagger) == 1);
    CHECK(inv.remove_extra(k_player, k_dagger, 1, 1) == 0);
    inv.add(k_player, k_dagger, 1, 3);
    const auto moved = inv.remove_extra(k_player, k_dagger, 3, 1);
    inv.add(k_chest, k_dagger, moved, 3);
    CHECK(inv.count_extra(k_chest, k_dagger, 3) == 1);
    CHECK(inv.count_extra(k_player, k_dagger, 0) == 1);
    inv.erase(k_chest);
    CHECK(inv.count(k_chest, k_dagger) == 0);
    CHECK(inv.extras_of(k_chest, k_dagger).empty());
}

TEST_CASE("equipment remembers the extra of what it holds", "[items][extras]") {
    vm::ItemInfo sword;
    sword.known = true;
    sword.form = 0x10;
    sword.category = vm::ItemCategory::weapon;
    sword.equip_slot = vm::etyp::right_hand;
    sword.weapon_type = 1;
    vm::ItemInfo cuirass;
    cuirass.known = true;
    cuirass.form = 0x20;
    cuirass.category = vm::ItemCategory::armor;
    cuirass.biped_slots = 1U << 2;
    const auto lookup = [&](std::uint32_t form) { return form == 0x10 ? sword : cuirass; };
    vm::Equipment e;
    CHECK(vm::equip(e, sword, false, lookup, 4).action == vm::EquipAction::equipped);
    CHECK(vm::equip(e, cuirass, false, lookup, 5).action == vm::EquipAction::equipped);
    CHECK(e.right_extra == 4);
    CHECK(e.holds(0x10, 4));
    CHECK_FALSE(e.holds(0x10, 0));
    CHECK(e.extra_of(0x20) == 5);
    CHECK_FALSE(vm::unequip(e, 0x10, 3)); // another stack
    CHECK(vm::unequip(e, 0x10, 4));
    CHECK(e.right_extra == 0);
    CHECK(vm::unequip(e, 0x20));
    CHECK(e.worn_extra.empty());
}

// ---- ITST and CRFT -----------------------------------------------------------------

TEST_CASE("ITST version 3 round-trips extras, equipped extras and dropped extras", "[items][save]") {
    vm::ItemStates s;
    const auto id = s.extras().intern([] {
        vm::ItemExtra e;
        e.health = 1.3F;
        e.enchantment = 0x77;
        e.name = "Blade";
        return e;
    }());
    s.mark_resolved(k_player);
    auto& eq = s.equipment(k_player);
    eq.right = k_dagger;
    eq.right_extra = id;
    eq.worn.insert(0x20);
    eq.worn_extra[0x20] = id;
    s.set_dropped(0x5000, 3, id);
    vm::Inventory inv;
    inv.add(k_player, k_dagger, 2);
    inv.add(k_player, k_dagger, 1, id);

    vm::ItemStates back;
    REQUIRE(vm::ItemStates::load(skydot::save::ByteReader(s.save(inv)), back));
    const auto* extra = back.extras().find(id);
    REQUIRE(extra != nullptr);
    CHECK(extra->health == Approx(1.3F));
    CHECK(extra->enchantment == 0x77);
    CHECK(extra->name == "Blade");
    REQUIRE(back.find(k_player) != nullptr);
    CHECK(back.find(k_player)->right_extra == id);
    CHECK(back.find(k_player)->worn_extra.at(0x20) == id);
    CHECK(back.dropped_count(0x5000) == 3);
    CHECK(back.dropped_extra(0x5000) == id);
    vm::Inventory loaded;
    loaded.add(k_player, k_dagger, 3);
    loaded.set_splits(back.take_splits());
    CHECK(loaded.count_extra(k_player, k_dagger, id) == 1);
    CHECK(loaded.count_extra(k_player, k_dagger, 0) == 2);
}

TEST_CASE("an ITST of version 2 still loads, every unit plain", "[items][save]") {
    skydot::save::ByteWriter w;
    w.u32(1); // resolved owners
    w.u32(k_player);
    w.u32(0); // picks
    w.u32(1); // equipment
    w.u32(k_player);
    w.u32(k_dagger); // right
    w.u32(0);
    w.u32(0);
    w.u32(1); // worn
    w.u32(0x20);
    w.u32(1); // dropped
    w.u32(0x5000);
    w.u64(4);
    vm::ItemStates s;
    REQUIRE(vm::ItemStates::load(skydot::save::ByteReader(w.out), s, 2));
    CHECK(s.resolved(k_player));
    CHECK(s.find(k_player)->right == k_dagger);
    CHECK(s.find(k_player)->right_extra == 0);
    CHECK(s.dropped_count(0x5000) == 4);
    CHECK(s.dropped_extra(0x5000) == 0);
    CHECK(s.extras().size() == 0);
    CHECK(s.take_splits().empty());
    // The same bytes are not a version 3.
    vm::ItemStates other;
    CHECK_FALSE(vm::ItemStates::load(skydot::save::ByteReader(w.out), other, 3));
}

TEST_CASE("created potions are deduplicated by effects, and CRFT round-trips with the knowledge", "[items][save]") {
    vm::CreatedForms created;
    vm::CreatedPotion a;
    a.name = "Potion of Restore Health";
    a.value = 58;
    a.effects = {{0xE1, 21.5F, 0}};
    const auto id = created.create(a);
    CHECK(id == 0xFF700000u);
    CHECK(created.is_created(id));
    CHECK(created.create(a) == id); // the same effects, the same form
    vm::CreatedPotion b = a;
    b.effects = {{0xE1, 10.0F, 0}};
    const auto other = created.create(b);
    CHECK(other == id + 1);
    created.learn(0x30, 0);
    created.learn(0x30, 2);
    CHECK(created.known(0x30, 2));
    CHECK_FALSE(created.known(0x30, 1));

    vm::CreatedForms back;
    REQUIRE(vm::CreatedForms::load(skydot::save::ByteReader(created.save()), back));
    REQUIRE(back.find(id) != nullptr);
    CHECK(back.find(id)->name == "Potion of Restore Health");
    CHECK(back.find(id)->effects[0].magnitude == Approx(21.5F));
    CHECK(back.known_mask(0x30) == 0b101);
    CHECK(back.create(b) == other);
    vm::CreatedPotion c = a;
    c.effects = {{0xE9, 1.0F, 5}};
    CHECK(back.create(c) == id + 2); // ids go on after the saved ones
}

// ---- harvest ------------------------------------------------------------------------

TEST_CASE("seasons follow the months and PFPC has a byte for each", "[harvest]") {
    CHECK(vm::season_of_month(2) == vm::Season::spring);
    CHECK(vm::season_of_month(4) == vm::Season::spring);
    CHECK(vm::season_of_month(5) == vm::Season::summer);
    CHECK(vm::season_of_month(7) == vm::Season::summer);
    CHECK(vm::season_of_month(8) == vm::Season::autumn);
    CHECK(vm::season_of_month(10) == vm::Season::autumn);
    CHECK(vm::season_of_month(11) == vm::Season::winter);
    CHECK(vm::season_of_month(0) == vm::Season::winter);
    CHECK(vm::season_of_month(1) == vm::Season::winter);
    const std::uint32_t yield = 10U | (20U << 8) | (30U << 16) | (40U << 24);
    CHECK(vm::harvest_chance(yield, vm::Season::spring) == 10);
    CHECK(vm::harvest_chance(yield, vm::Season::summer) == 20);
    CHECK(vm::harvest_chance(yield, vm::Season::autumn) == 30);
    CHECK(vm::harvest_chance(yield, vm::Season::winter) == 40);
}

TEST_CASE("the chance is a percentage, the same for a reference and count", "[harvest]") {
    CHECK_FALSE(vm::harvest_succeeds(0, 0xC0533, 0));
    CHECK(vm::harvest_succeeds(100, 0xC0533, 0));
    int hits = 0;
    for (std::uint32_t ref = 1; ref <= 1000; ++ref) {
        hits += vm::harvest_succeeds(30, ref, 0) ? 1 : 0;
        CHECK(vm::harvest_succeeds(30, ref, 0) == vm::harvest_succeeds(30, ref, 0));
    }
    CHECK(hits > 220);
    CHECK(hits < 380);
}

TEST_CASE("a harvested reference respawns after ten game days, and HRVS round-trips", "[harvest][save]") {
    vm::HarvestStates s;
    CHECK_FALSE(s.harvested(0x10, 5.0));
    s.mark(0x10, 5.0);
    CHECK(s.harvested(0x10, 5.0));
    CHECK(s.harvested(0x10, 14.9));
    CHECK_FALSE(s.harvested(0x10, 15.0));
    CHECK(s.times(0x10) == 1);
    s.mark(0x10, 20.0);
    CHECK(s.times(0x10) == 2);

    vm::HarvestStates back;
    REQUIRE(vm::HarvestStates::load(skydot::save::ByteReader(s.save()), back));
    CHECK(back.harvested(0x10, 25.0));
    CHECK_FALSE(back.harvested(0x10, 30.0));
    CHECK(back.times(0x10) == 2);
    auto bytes = s.save();
    bytes.pop_back();
    CHECK_FALSE(vm::HarvestStates::load(skydot::save::ByteReader(bytes), back));
}

TEST_CASE("the harvested references are listed without the respawned ones", "[harvest]") {
    vm::HarvestStates s;
    CHECK(s.harvested_refs(0.0).empty());
    s.mark(0x20, 1.0);
    s.mark(0x10, 5.0);
    CHECK(s.harvested_refs(6.0) == std::vector<std::uint32_t>{0x10, 0x20});
    CHECK(s.harvested_refs(12.0) == std::vector<std::uint32_t>{0x10});
    CHECK(s.harvested_refs(15.0).empty());
}

// ---- crafting ------------------------------------------------------------------------

namespace {

constexpr std::uint32_t k_forge_kw = 0x100;
constexpr std::uint32_t k_wheel_kw = 0x101;
constexpr std::uint32_t k_table_kw = 0x102;
constexpr std::uint32_t k_forge = 0x2000;
constexpr std::uint32_t k_wheel = 0x2001;
constexpr std::uint32_t k_ingot = 0x10;
constexpr std::uint32_t k_leather = 0x11;
constexpr std::uint32_t k_strips = 0x12;
constexpr std::uint32_t k_a = 0x30;
constexpr std::uint32_t k_b = 0x31;
constexpr std::uint32_t k_c = 0x32;
constexpr std::uint32_t k_d = 0x33;
constexpr std::uint32_t k_e = 0x34;
constexpr std::uint32_t k_heal = 0xE1;
constexpr std::uint32_t k_fortify = 0xE2;
constexpr std::uint32_t k_harm = 0xE3;
constexpr std::uint32_t k_make_dagger = 0x500;
constexpr std::uint32_t k_make_strips = 0x501;
constexpr std::uint32_t k_hidden = 0x502;
constexpr std::uint32_t k_temper_dagger = 0x600;

ItemSpec misc() {
    ItemSpec i;
    i.value = 5;
    return i;
}
ItemSpec ingredient(std::vector<ItemSpec::Effect> effects) {
    ItemSpec i;
    i.category = wfb::ItemCategory::ingredient;
    i.weight = 0.1F;
    i.value = 2;
    i.effects = std::move(effects);
    return i;
}

WorldSpec crafting_world() {
    WorldSpec w;
    ItemSpec dagger;
    dagger.category = wfb::ItemCategory::weapon;
    dagger.value = 20;
    BaseSpec forge{.id = k_forge, .name = "Forge", .keywords = {k_forge_kw}, .bench_type = 1};
    BaseSpec wheel{.id = k_wheel, .name = "Grindstone", .keywords = {k_wheel_kw}, .bench_type = 2};
    w.bases = {
        forge,
        wheel,
        {.id = k_ingot, .name = "Iron Ingot", .item = misc()},
        {.id = k_leather, .name = "Leather", .item = misc()},
        {.id = k_strips, .name = "Leather Strips", .item = misc()},
        {.id = k_dagger, .name = "Iron Dagger", .item = dagger},
        {.id = k_a, .name = "Alpha", .item = ingredient({{k_heal, 5.0F, 0}, {k_fortify, 1.0F, 60}})},
        {.id = k_b, .name = "Beta", .item = ingredient({{k_heal, 3.0F, 0}, {k_harm, 2.0F, 10}})},
        {.id = k_c, .name = "Gamma", .item = ingredient({{k_fortify, 1.0F, 30}})},
        {.id = k_d, .name = "Delta", .item = ingredient({{k_harm, 4.0F, 5}})},
        {.id = k_e, .name = "Epsilon", .item = ingredient({{k_harm, 1.0F, 5}, {k_heal, 1.0F, 0}})},
    };
    w.magic_effects = {
        {.id = k_heal, .name = "Restore Health", .description = "Restore <mag> points of Health.",
         .flags = 0x200000, .base_cost = 2.0F},
        {.id = k_fortify, .name = "Fortify Test", .description = "For <dur> seconds.", .flags = 0x400000,
         .base_cost = 1.0F},
        {.id = k_harm, .name = "Damage Test", .flags = 1, .base_cost = 3.0F},
    };
    w.recipes = {
        {.id = k_make_dagger, .product = k_dagger, .bench_keyword = k_forge_kw,
         .components = {{k_ingot, 2}, {k_strips, 1}}},
        {.id = k_make_strips, .product = k_strips, .product_count = 2, .bench_keyword = k_forge_kw,
         .components = {{k_leather, 1}}},
        {.id = k_hidden, .product = k_ingot, .bench_keyword = k_forge_kw, .components = {{k_ingot, 1}}},
        {.id = k_temper_dagger, .product = k_dagger, .bench_keyword = k_wheel_kw, .components = {{k_ingot, 1}}},
    };
    return w;
}

struct FakeHost final : crafting::CraftingHost {
    std::map<std::uint32_t, int> types{{k_forge, 1}, {k_wheel, 2}};
    std::map<std::uint32_t, std::set<std::uint32_t>> keywords{{k_forge, {k_forge_kw}}, {k_wheel, {k_wheel_kw}}};
    bool deny = false; // every recipe's conditions fail
    std::vector<std::string> lines;
    std::vector<bool> enchanted_asked;

    bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>*,
                         const crafting::ConditionItem* item) override {
        if (item != nullptr) {
            enchanted_asked.push_back(item->enchanted);
        }
        return !deny;
    }
    int bench_type(std::uint32_t ref) const override {
        const auto it = types.find(ref);
        return it == types.end() ? 0 : it->second;
    }
    bool bench_has_keyword(std::uint32_t ref, std::uint32_t keyword) const override {
        const auto it = keywords.find(ref);
        return it != keywords.end() && it->second.contains(keyword);
    }
    std::uint32_t keyword(std::string_view id) const override {
        if (id == crafting::k_weapon_tempering_keyword) {
            return k_wheel_kw;
        }
        return id == crafting::k_armor_tempering_keyword ? k_table_kw : 0;
    }
    void log(const std::string& line) override { lines.push_back(line); }
};

struct Rig {
    BuiltWorld world{crafting_world()};
    FakeHost host;
    vm::Inventory inventory;
    vm::ItemExtras extras;
    vm::CreatedForms created;
    crafting::Crafting crafting{*world, host, inventory, extras, created, k_player};
};

} // namespace

TEST_CASE("a forge lists the recipes of its keyword, with what is held", "[crafting]") {
    Rig r;
    r.inventory.add(k_player, k_ingot, 3);
    r.inventory.add(k_player, k_strips, 1);
    const auto rows = r.crafting.recipes(k_forge);
    REQUIRE(rows.size() == 3);
    const auto& dagger = rows[0].recipe == k_make_dagger ? rows[0] : rows[1];
    CHECK(dagger.product == k_dagger);
    REQUIRE(dagger.components.size() == 2);
    CHECK((dagger.components[0].need == 2 && dagger.components[0].have == 3));
    CHECK(dagger.craftable);
    CHECK_FALSE((rows[0].recipe == k_make_strips ? rows[0].craftable : rows[1].craftable)); // no leather
    CHECK(r.crafting.recipes(k_wheel).empty()); // a grindstone makes nothing
    CHECK(r.crafting.recipes(0x9999).empty());  // not a bench
}

TEST_CASE("a recipe whose conditions fail is not listed and cannot be made", "[crafting]") {
    Rig r;
    r.inventory.add(k_player, k_ingot, 3);
    r.inventory.add(k_player, k_strips, 1);
    r.host.deny = true;
    CHECK(r.crafting.recipes(k_forge).empty());
    CHECK(r.crafting.craft(k_make_dagger, 1) == 0);
    r.inventory.add(k_player, k_dagger, 1);
    CHECK(r.crafting.temper_rows(k_wheel).empty());
    CHECK_FALSE(r.crafting.temper(k_temper_dagger, k_dagger, 0));
}

TEST_CASE("tempering asks the conditions whether the item is enchanted", "[crafting]") {
    Rig r;
    r.inventory.add(k_player, k_dagger, 1);
    vm::ItemExtra e;
    e.enchantment = 0x99;
    r.inventory.add(k_player, k_dagger, 1, r.extras.intern(e));
    (void)r.crafting.temper_rows(k_wheel);
    REQUIRE(r.host.enchanted_asked.size() == 2);
    CHECK_FALSE(r.host.enchanted_asked[0]);
    CHECK(r.host.enchanted_asked[1]);
}

TEST_CASE("crafting takes the components and gives the product count", "[crafting]") {
    Rig r;
    r.inventory.add(k_player, k_leather, 3);
    r.inventory.add(k_player, k_ingot, 2);
    CHECK(r.crafting.craft(k_make_strips, 5) == 3); // as many as the components allow
    CHECK(r.inventory.count(k_player, k_strips) == 6);
    CHECK(r.inventory.count(k_player, k_leather) == 0);
    CHECK(r.crafting.craft(k_make_dagger, 1) == 1);
    CHECK(r.inventory.count(k_player, k_dagger) == 1);
    CHECK(r.inventory.count(k_player, k_ingot) == 0);
    CHECK(r.inventory.count(k_player, k_strips) == 5);
    REQUIRE_FALSE(r.host.lines.empty());
    CHECK(r.host.lines.back().find("skill: Smithing use") == 0);
    CHECK(r.host.lines.back().find("value 20") != std::string::npos);
    CHECK(r.crafting.craft(0xDEAD, 1) == 0);
}

TEST_CASE("tempering needs a carried product and only improves it", "[crafting]") {
    Rig r;
    CHECK(r.crafting.temper_rows(k_wheel).empty());
    r.inventory.add(k_player, k_dagger, 2);
    r.inventory.add(k_player, k_ingot, 2);
    auto rows = r.crafting.temper_rows(k_wheel);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].recipe == k_temper_dagger);
    CHECK(rows[0].extra == 0);
    CHECK(rows[0].count == 2);
    CHECK(rows[0].health == Approx(1.0F));
    CHECK(rows[0].next_health == Approx(1.1F));
    CHECK(rows[0].craftable);
    CHECK(std::string(crafting::Crafting::tier_name(rows[0].next_health)) == "Fine");
    CHECK(r.crafting.temper_rows(k_forge).empty()); // not a smithing bench

    CHECK(r.crafting.temper(k_temper_dagger, k_dagger, 0));
    CHECK(r.inventory.count(k_player, k_dagger) == 2); // one of them is better now
    CHECK(r.inventory.count_extra(k_player, k_dagger, 0) == 1);
    CHECK(r.inventory.count(k_player, k_ingot) == 1);
    rows = r.crafting.temper_rows(k_wheel);
    REQUIRE(rows.size() == 2);
    const auto& tempered = rows[0].extra != 0 ? rows[0] : rows[1];
    CHECK(tempered.health == Approx(1.1F));
    CHECK_FALSE(tempered.craftable); // already Fine: the placeholder gives no more
    CHECK_FALSE(r.crafting.temper(k_temper_dagger, k_dagger, tempered.extra));
    CHECK(r.inventory.count(k_player, k_ingot) == 1);
    // A second plain dagger tempered joins the Fine stack.
    CHECK(r.crafting.temper(k_temper_dagger, k_dagger, 0));
    CHECK(r.inventory.count_extra(k_player, k_dagger, tempered.extra) == 2);
    REQUIRE_FALSE(r.host.lines.empty());
    CHECK(r.host.lines.back().find("Smithing") != std::string::npos);
}

TEST_CASE("the enchanting stub lists enchanted items only", "[crafting]") {
    Rig r;
    r.inventory.add(k_player, k_dagger, 1);
    CHECK(r.crafting.enchant_rows().empty());
    vm::ItemExtra e;
    e.enchantment = 0x99;
    r.inventory.add(k_player, k_dagger, 1, r.extras.intern(e));
    const auto rows = r.crafting.enchant_rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].enchantment == 0x99);
}

TEST_CASE("ingredients sharing an effect make a potion, the strongest instance wins", "[crafting][alchemy]") {
    Rig r;
    r.inventory.add(k_player, k_a, 2);
    r.inventory.add(k_player, k_b, 1);
    r.inventory.add(k_player, k_c, 1);
    const std::vector<std::uint32_t> ab{k_a, k_b};
    const auto plan = r.crafting.preview(ab);
    REQUIRE(plan.mixable);
    REQUIRE(plan.effects.size() == 1);
    CHECK(plan.effects[0].effect == k_heal);
    // Power 4 * (1 + 0.5 * 15 / 100) = 4.3; Restore Health scales its magnitude (A's 5 wins).
    CHECK(plan.effects[0].magnitude == Approx(21.5F));
    CHECK(plan.effects[0].duration == 0);
    CHECK(plan.name == "Potion of Restore Health");
    CHECK_FALSE(plan.poison);
    CHECK(plan.value == std::lround(2.0 * std::pow(21.5, 1.1)));
    CHECK(r.crafting.describe(k_heal, 21.5F, 0) == "Restore 22 points of Health.");

    CHECK_FALSE(r.crafting.preview(std::vector<std::uint32_t>{k_a, k_a}).mixable); // the same twice
    CHECK_FALSE(r.crafting.preview(std::vector<std::uint32_t>{k_a}).mixable);
    CHECK_FALSE(r.crafting.preview(std::vector<std::uint32_t>{k_b, k_c}).mixable); // nothing shared
    CHECK(r.crafting.preview(std::vector<std::uint32_t>{k_a, k_b, k_c}).effects.size() == 2); // heal, fortify
    const auto fortify = r.crafting.preview(std::vector<std::uint32_t>{k_a, k_c});
    REQUIRE(fortify.mixable);
    CHECK(fortify.effects[0].duration == static_cast<std::uint32_t>(std::lround(60 * 4.3))); // power on duration
}

TEST_CASE("hostile effects make poisons", "[crafting][alchemy]") {
    Rig r;
    const auto plan = r.crafting.preview(std::vector<std::uint32_t>{k_d, k_e});
    REQUIRE(plan.mixable);
    CHECK(plan.poison);
    CHECK(plan.name == "Poison of Damage Test");
}

TEST_CASE("mixing consumes one of each, creates the potion once and teaches the shared effects", "[crafting][alchemy]") {
    Rig r;
    r.inventory.add(k_player, k_a, 2);
    r.inventory.add(k_player, k_b, 2);
    const std::vector<std::uint32_t> ab{k_a, k_b};
    const auto id = r.crafting.mix(ab);
    REQUIRE(id != 0);
    CHECK(r.created.is_created(id));
    CHECK(r.created.find(id)->name == "Potion of Restore Health");
    CHECK(r.created.find(id)->weight == Approx(0.5F));
    CHECK(r.inventory.count(k_player, id) == 1);
    CHECK(r.inventory.count(k_player, k_a) == 1);
    CHECK(r.inventory.count(k_player, k_b) == 1);
    CHECK(r.created.known(k_a, 0)); // Restore Health is A's first effect
    CHECK_FALSE(r.created.known(k_a, 1));
    CHECK(r.created.known(k_b, 0));
    REQUIRE_FALSE(r.host.lines.empty());
    CHECK(r.host.lines.back().find("skill: Alchemy use") == 0);
    // Again: the same form, now two of them.
    CHECK(r.crafting.mix(ab) == id);
    CHECK(r.inventory.count(k_player, id) == 2);
    // Nothing left to mix.
    CHECK(r.crafting.mix(ab) == 0);

    const auto rows = r.crafting.alchemy_rows();
    CHECK(rows.empty()); // all four were used
    r.inventory.add(k_player, k_a, 1);
    const auto after = r.crafting.alchemy_rows();
    REQUIRE(after.size() == 1);
    REQUIRE(after[0].effects.size() == 2);
    CHECK(after[0].effects[0].known);
    CHECK_FALSE(after[0].effects[1].known);
}

TEST_CASE("eating an ingredient teaches its first effect", "[crafting][alchemy]") {
    Rig r;
    r.crafting.note_eaten(k_c);
    CHECK(r.created.known(k_c, 0));
    r.crafting.note_eaten(k_dagger); // no ingredient: nothing
    CHECK(r.created.known_mask(k_dagger) == 0);
}
