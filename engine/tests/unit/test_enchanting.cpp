// SPDX-License-Identifier: GPL-3.0-or-later
//
// The enchanting table (GAMEPLAY-PLAN 7a-6, docs/adr/0008): the UESP formulas at skill 15, what fits
// what, disenchanting, enchanting, recharging, filling soul gems, and the saves of created
// enchantments and soul extras.
#include "support/world_builder.hpp"

#include "data/fb_search.hpp"
#include "magic/enchanting.hpp"
#include "save/bytes.hpp"
#include "vm/created_forms.hpp"
#include "vm/inventory.hpp"
#include "vm/item_extras.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

using namespace skydot::testing;
namespace magic = skydot::magic;
namespace vm = skydot::vm;
using Catch::Approx;

namespace {

constexpr std::uint32_t k_player = 0x14;

constexpr std::uint32_t k_mg_fire = 0x1001;     // magnitude
constexpr std::uint32_t k_mg_fortify = 0x1002;  // magnitude
constexpr std::uint32_t k_mg_invis = 0x1003;    // power affects duration
constexpr std::uint32_t k_mg_banned = 0x1004;   // MagicDisallowEnchanting

constexpr std::uint32_t k_kw_disallow = 0x50;
constexpr std::uint32_t k_kw_reusable = 0x51;
constexpr std::uint32_t k_kw_sword = 0x52;
constexpr std::uint32_t k_list_swords = 0x60;

constexpr std::uint32_t k_en_fire = 0x4001;      // weapon, manual cost 100
constexpr std::uint32_t k_en_fortify = 0x4002;   // apparel, 20
constexpr std::uint32_t k_en_invis = 0x4003;     // apparel, duration 60
constexpr std::uint32_t k_en_staff = 0x4004;     // staff enchantment
constexpr std::uint32_t k_en_banned = 0x4005;    // apparel with a banned effect
constexpr std::uint32_t k_en_sword_only = 0x4006; // weapon, worn restrictions: swords
constexpr std::uint32_t k_en_fire_big = 0x4007;  // made from k_en_fire
constexpr std::uint32_t k_en_cheap = 0x4008;     // weapon, manual cost 10

constexpr std::uint32_t k_sword = 0x3001;
constexpr std::uint32_t k_dagger = 0x3002;
constexpr std::uint32_t k_staff = 0x3003;
constexpr std::uint32_t k_ring = 0x3004;
constexpr std::uint32_t k_found_sword = 0x3005;  // EITM k_en_fire_big
constexpr std::uint32_t k_banned_sword = 0x3006; // keyword MagicDisallowEnchanting, EITM k_en_fire
constexpr std::uint32_t k_staff_item = 0x3007;   // EITM k_en_staff
constexpr std::uint32_t k_banned_ring = 0x3008;  // EITM k_en_banned
constexpr std::uint32_t k_fists = 0x3009;
constexpr std::uint32_t k_misc = 0x300A;

constexpr std::uint32_t k_petty = 0x5001, k_petty_full = 0x5002;
constexpr std::uint32_t k_grand = 0x5003, k_grand_full = 0x5004;
constexpr std::uint32_t k_black = 0x5005, k_black_full = 0x5006;
constexpr std::uint32_t k_azura = 0x5007;
constexpr std::uint32_t k_lesser = 0x5008, k_lesser_full = 0x5009;
constexpr std::uint32_t k_reuse_full = 0x500A, k_reuse = 0x500B;
constexpr std::uint32_t k_common = 0x500C;

WorldSpec world_spec() {
    WorldSpec spec;
    spec.format_version = 35;
    spec.keywords = {{k_kw_disallow, "MagicDisallowEnchanting"}, {k_kw_reusable, "ReusableSoulGem"},
                     {k_kw_sword, "WeapTypeSword"}};
    spec.form_lists = {{k_list_swords, {k_kw_sword}}};
    const auto mgef = [](std::uint32_t id, const char* name, std::uint32_t flags, float cost) {
        MagicEffectSpec m{.id = id};
        m.name = name;
        m.flags = flags;
        m.base_cost = cost;
        return m;
    };
    auto banned = mgef(k_mg_banned, "Banned", 0, 1.0F);
    banned.keywords = {k_kw_disallow};
    spec.magic_effects = {mgef(k_mg_fire, "Fire Damage", 0x1, 1.0F), mgef(k_mg_fortify, "Fortify Health", 0, 1.0F),
                          mgef(k_mg_invis, "Invisibility", 0x400000, 1.0F), banned};
    const auto ench = [](std::uint32_t id, const char* name, std::int32_t cost, std::uint32_t casting,
                         std::uint32_t effect, float magnitude, std::uint32_t duration = 0) {
        return EnchantmentSpec{.id = id, .name = name, .cost = cost, .charge = 0, .flags = 0x1, .casting_type = casting,
                               .effects = {{.effect = effect, .magnitude = magnitude, .duration = duration}}};
    };
    auto staff = ench(k_en_staff, "Staff Fire", 10, 1, k_mg_fire, 5.0F);
    staff.enchantment_type = 12;
    auto sword_only = ench(k_en_sword_only, "Sword Fire", 100, 1, k_mg_fire, 10.0F);
    sword_only.worn_restrictions = k_list_swords;
    auto big = ench(k_en_fire_big, "Greater Fire", 200, 1, k_mg_fire, 20.0F);
    big.base_enchantment = k_en_fire;
    spec.enchantments = {ench(k_en_fire, "Fire Damage", 100, 1, k_mg_fire, 10.0F),
                         ench(k_en_fortify, "Fortify Health", 0, 0, k_mg_fortify, 20.0F),
                         ench(k_en_invis, "Invisibility", 0, 0, k_mg_invis, 1.0F, 60),
                         staff,
                         ench(k_en_banned, "Banned", 0, 0, k_mg_banned, 5.0F),
                         sword_only,
                         big,
                         ench(k_en_cheap, "Cheap Fire", 10, 1, k_mg_fire, 4.0F)};
    const auto weapon = [](std::uint32_t id, const char* name, std::uint8_t type, std::uint32_t ench_id = 0,
                           std::vector<std::uint32_t> kw = {}) {
        BaseSpec b{.id = id, .name = name};
        b.item = ItemSpec{.category = wfb::ItemCategory::weapon, .weapon_type = type, .damage = 7.0F, .enchantment = ench_id};
        b.keywords = std::move(kw);
        return b;
    };
    const auto ring = [](std::uint32_t id, const char* name, std::uint32_t ench_id = 0) {
        BaseSpec b{.id = id, .name = name};
        b.item = ItemSpec{.category = wfb::ItemCategory::jewelry, .enchantment = ench_id};
        return b;
    };
    const auto gem = [](std::uint32_t id, const char* name, std::uint8_t soul, std::uint8_t cap, std::uint32_t link,
                        bool npc, std::vector<std::uint32_t> kw = {}) {
        BaseSpec b{.id = id, .name = name};
        b.item = ItemSpec{.category = wfb::ItemCategory::soul_gem, .soul = soul, .soul_capacity = cap,
                          .soul_gem_linked = link, .holds_npc_souls = npc};
        b.keywords = std::move(kw);
        return b;
    };
    BaseSpec misc{.id = k_misc, .name = "Rock"};
    misc.item = ItemSpec{.category = wfb::ItemCategory::misc};
    spec.bases = {weapon(k_sword, "Iron Sword", 1, 0, {k_kw_sword}),
                  weapon(k_dagger, "Iron Dagger", 2),
                  weapon(k_staff, "Staff", 8),
                  ring(k_ring, "Gold Ring"),
                  weapon(k_found_sword, "Found Sword", 1, k_en_fire_big, {k_kw_sword}),
                  weapon(k_banned_sword, "Banned Sword", 1, k_en_fire, {k_kw_disallow}),
                  weapon(k_staff_item, "Fire Staff", 8, k_en_staff),
                  ring(k_banned_ring, "Banned Ring", k_en_banned),
                  weapon(k_fists, "Fists", 0),
                  misc,
                  gem(k_petty, "Petty Soul Gem", 0, 1, 0, false),
                  gem(k_petty_full, "Petty Soul Gem", 1, 1, 0, false),
                  gem(k_grand, "Grand Soul Gem", 0, 5, k_grand_full, false),
                  gem(k_grand_full, "Grand Soul Gem", 5, 5, 0, false),
                  gem(k_black, "Black Soul Gem", 0, 5, k_black_full, true),
                  gem(k_black_full, "Black Soul Gem", 5, 5, 0, true),
                  gem(k_azura, "Azura's Star", 0, 5, 0, false, {k_kw_reusable}),
                  gem(k_lesser, "Lesser Soul Gem", 0, 2, k_lesser_full, false),
                  gem(k_lesser_full, "Lesser Soul Gem", 2, 2, 0, false),
                  gem(k_reuse_full, "Varla Stone", 5, 5, 0, false, {k_kw_reusable}),
                  gem(k_reuse, "Varla Stone", 0, 5, k_reuse_full, false, {k_kw_reusable}),
                  gem(k_common, "Common Soul Gem", 0, 3, 0, false)};
    return spec;
}

struct StubHost : magic::MagicHost {
    double actor_value(std::uint32_t, std::uint32_t) const override { return 0.0; }
    void damage_value(std::uint32_t, std::uint32_t, double, std::uint32_t) override {}
    void restore_value(std::uint32_t, std::uint32_t, double) override {}
    void modify_temp(std::uint32_t, std::uint32_t, double) override {}
    bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<magic::wfb::Condition>>*, std::uint32_t,
                         std::uint32_t) override {
        return true;
    }
    bool is_dead(std::uint32_t) const override { return false; }
    void log(const std::string&) override {}
};

struct Rig {
    BuiltWorld world{world_spec()};
    StubHost host;
    magic::ActiveEffects effects{*world, host};
    magic::SpellBook book{*world, effects};
    vm::Inventory inventory;
    vm::ItemExtras extras;
    vm::CreatedForms created;
    magic::Enchanting table{*world, inventory, extras, created, book, {k_kw_disallow, k_kw_reusable}, k_player};

    Rig() {
        book.learn_enchantment(k_en_fire);
        book.learn_enchantment(k_en_fortify);
        book.learn_enchantment(k_en_invis);
        book.learn_enchantment(k_en_sword_only);
        book.learn_enchantment(k_en_cheap);
    }
    [[nodiscard]] std::int64_t have(std::uint32_t form) const { return inventory.count(k_player, form); }
    [[nodiscard]] magic::EnchantRequest request(std::uint32_t item, std::uint32_t ench, std::uint32_t gem) const {
        return {.item = item, .item_extra = 0, .enchantment = ench, .gem = gem, .gem_extra = 0, .name = {}};
    }
};

} // namespace

// ---- formulas ------------------------------------------------------------------------------------

TEST_CASE("the enchanting formulas at skill 15", "[enchanting]") {
    CHECK(magic::enchant_skill_multiplier(15) == Approx(1.0 + 0.15 * (0.15 - 0.14) / 3.4));
    CHECK(magic::enchant_skill_multiplier(100) == Approx(1.0 + 0.86 / 3.4));
    CHECK(magic::soul_charge(1) == 250);
    CHECK(magic::soul_charge(3) == 1000);
    CHECK(magic::soul_charge(5) == 3000);
    CHECK(magic::soul_charge(0) == 0);
    CHECK(magic::soul_magnitude_multiplier(5) == Approx(1.0));
    CHECK(magic::soul_magnitude_multiplier(4) == Approx(2.0 / 3.0));
    CHECK(magic::soul_magnitude_multiplier(3) == Approx(1.0 / 3.0));
    CHECK(magic::soul_magnitude_multiplier(2) == Approx(1.0 / 6.0));
    CHECK(magic::soul_magnitude_multiplier(1) == Approx(1.0 / 12.0));
    CHECK(magic::weapon_use_cost(100.0, 15.0) == Approx(3.0 * std::pow(100.0, 1.1) * (1.0 - std::sqrt(15.0 / 200.0))));
    CHECK(magic::weapon_use_cost(0.0, 15.0) == 0.0);
}

TEST_CASE("a creature's soul follows its level, an NPC's is black and grand", "[enchanting]") {
    CHECK(magic::actor_soul(1, false).size == 1);
    CHECK(magic::actor_soul(3, false).size == 1);
    CHECK(magic::actor_soul(4, false).size == 2);
    CHECK(magic::actor_soul(15, false).size == 2);
    CHECK(magic::actor_soul(16, false).size == 3);
    CHECK(magic::actor_soul(28, false).size == 4);
    CHECK(magic::actor_soul(37, false).size == 4);
    CHECK(magic::actor_soul(38, false).size == 5);
    CHECK_FALSE(magic::actor_soul(60, false).npc);
    CHECK(magic::actor_soul(1, true).size == 5);
    CHECK(magic::actor_soul(1, true).npc);
}

TEST_CASE("apparel magnitudes follow the soul, duration effects scale their duration", "[enchanting]") {
    Rig rig;
    const auto* fortify = skydot::lookup(rig.world->enchantments(), k_en_fortify);
    const auto* invis = skydot::lookup(rig.world->enchantments(), k_en_invis);
    REQUIRE((fortify != nullptr && invis != nullptr));
    const auto magnitude = [&](std::uint8_t soul) {
        const auto made = magic::make_enchantment(*rig.world, *fortify, magic::ItemKind::apparel, soul, 15.0);
        REQUIRE(made.has_value());
        CHECK(made->ench.constant);
        CHECK(made->ench.cost == 0.0);
        CHECK(made->charge_max == 0.0F);
        CHECK(made->ench.base == k_en_fortify);
        return made->ench.effects.at(0).magnitude;
    };
    CHECK(magnitude(5) == Approx(20.0F));
    CHECK(magnitude(4) == Approx(13.0F)); // floor(20 * 2/3 * 1.00044)
    CHECK(magnitude(3) == Approx(6.0F));
    CHECK(magnitude(2) == Approx(3.0F));
    CHECK(magnitude(1) == Approx(1.0F)); // at least 1

    const auto timed = magic::make_enchantment(*rig.world, *invis, magic::ItemKind::apparel, 4, 15.0);
    REQUIRE(timed.has_value());
    CHECK(timed->ench.effects.at(0).magnitude == Approx(1.0F));
    CHECK(timed->ench.effects.at(0).duration == 40); // floor(60 * 2/3 * 1.00044)
    CHECK_FALSE(magic::make_enchantment(*rig.world, *fortify, magic::ItemKind::apparel, 0, 15.0).has_value());
    CHECK_FALSE(magic::make_enchantment(*rig.world, *fortify, magic::ItemKind::staff, 5, 15.0).has_value());
}

TEST_CASE("a weapon enchantment holds the soul's charge and costs per use; a dear one shrinks to fit", "[enchanting]") {
    Rig rig;
    const auto* fire = skydot::lookup(rig.world->enchantments(), k_en_fire);
    REQUIRE(fire != nullptr);
    const auto lesser = magic::make_enchantment(*rig.world, *fire, magic::ItemKind::weapon, 2, 15.0);
    REQUIRE(lesser.has_value());
    CHECK_FALSE(lesser->ench.constant);
    CHECK(lesser->charge_max == Approx(500.0F));
    CHECK(lesser->ench.cost == Approx(magic::weapon_use_cost(100.0, 15.0)));
    CHECK(lesser->ench.cost < 500.0);
    CHECK(lesser->ench.effects.at(0).magnitude == Approx(10.0F)); // floor(10 * 1.00044)

    const auto petty = magic::make_enchantment(*rig.world, *fire, magic::ItemKind::weapon, 1, 15.0);
    REQUIRE(petty.has_value());
    CHECK(petty->charge_max == Approx(250.0F));
    CHECK(petty->ench.cost == Approx(250.0)); // one use fits the gem exactly
    CHECK(petty->ench.effects.at(0).magnitude < 10.0F);
    CHECK(petty->ench.effects.at(0).magnitude >= 1.0F);
}

// ---- fit ---------------------------------------------------------------------------------------

TEST_CASE("an enchantment fits an item of its kind and its worn restrictions", "[enchanting]") {
    Rig rig;
    const auto fits = [&](std::uint32_t ench, std::uint32_t item) {
        const auto* e = skydot::lookup(rig.world->enchantments(), ench);
        REQUIRE(e != nullptr);
        return magic::enchantment_fits(*rig.world, *e, item);
    };
    CHECK(fits(k_en_fire, k_sword));
    CHECK(fits(k_en_fire, k_dagger));
    CHECK_FALSE(fits(k_en_fire, k_ring));     // fire and forget is for weapons
    CHECK(fits(k_en_fortify, k_ring));
    CHECK_FALSE(fits(k_en_fortify, k_sword)); // constant is for apparel
    CHECK_FALSE(fits(k_en_staff, k_staff));   // a staff enchantment is no item enchantment
    CHECK_FALSE(fits(k_en_fire, k_staff));
    CHECK_FALSE(fits(k_en_fire, k_fists));
    CHECK_FALSE(fits(k_en_fire, k_misc));
    CHECK(fits(k_en_sword_only, k_sword));
    CHECK_FALSE(fits(k_en_sword_only, k_dagger));
    CHECK(magic::item_kind(*rig.world, k_staff) == magic::ItemKind::staff);
    CHECK(magic::item_kind(*rig.world, k_fists) == magic::ItemKind::none);
}

// ---- disenchanting -------------------------------------------------------------------------------

TEST_CASE("disenchanting destroys the unit and teaches the base enchantment", "[enchanting][disenchant]") {
    Rig rig;
    rig.book.clear(); // learned nothing yet
    rig.inventory.add(k_player, k_found_sword, 2);
    CHECK(rig.table.disenchant(k_found_sword, 0) == magic::Refusal::none);
    CHECK(rig.have(k_found_sword) == 1);
    CHECK(rig.book.knows_enchantment(k_en_fire)); // the base of the enchantment on the sword
    CHECK_FALSE(rig.book.knows_enchantment(k_en_fire_big));
    // The same enchantment again: already known.
    CHECK(rig.table.disenchant(k_found_sword, 0) == magic::Refusal::known);
    CHECK(rig.have(k_found_sword) == 1);
}

TEST_CASE("disenchanting refuses what has nothing to learn", "[enchanting][disenchant]") {
    Rig rig;
    rig.book.clear();
    for (const auto form : {k_sword, k_banned_sword, k_staff_item, k_banned_ring, k_found_sword}) {
        rig.inventory.add(k_player, form, 1);
    }
    CHECK(rig.table.disenchant(k_dagger, 0) == magic::Refusal::no_item);
    CHECK(rig.table.disenchant(k_sword, 0) == magic::Refusal::no_enchantment);
    CHECK(rig.table.disenchant(k_banned_sword, 0) == magic::Refusal::forbidden); // the item carries the keyword
    CHECK(rig.table.disenchant(k_banned_ring, 0) == magic::Refusal::forbidden);  // an effect carries it
    CHECK(rig.table.disenchant(k_staff_item, 0) == magic::Refusal::staff);
    CHECK(rig.book.known_enchantments().empty());
    CHECK(rig.have(k_banned_sword) == 1);

    // An enchantment the player made cannot be disenchanted.
    vm::ItemExtra made;
    made.enchantment = rig.created.create_enchantment({.id = 0, .base = k_en_fire, .name = "Fire", .effects = {{k_mg_fire, 5.0F, 0, 0}}, .cost = 20.0, .constant = false});
    const auto id = rig.extras.intern(made);
    rig.inventory.add(k_player, k_sword, 1, id);
    CHECK(rig.table.disenchant(k_sword, id) == magic::Refusal::created);
    CHECK(rig.have(k_sword) == 2);

    // An extra's enchantment on a base without one is the item's enchantment.
    vm::ItemExtra world_ench;
    world_ench.enchantment = k_en_fire;
    const auto wid = rig.extras.intern(world_ench);
    rig.inventory.add(k_player, k_dagger, 1, wid);
    CHECK(rig.table.disenchant(k_dagger, wid) == magic::Refusal::none);
    CHECK(rig.book.knows_enchantment(k_en_fire));
}

// ---- enchanting ----------------------------------------------------------------------------------

TEST_CASE("enchanting makes an extra with a created enchantment and spends the gem", "[enchanting][enchant]") {
    Rig rig;
    rig.inventory.add(k_player, k_ring, 2);
    rig.inventory.add(k_player, k_grand_full, 1);
    const auto out = rig.table.enchant(rig.request(k_ring, k_en_fortify, k_grand_full));
    REQUIRE(out.ok());
    CHECK(out.name == "Gold Ring of Fortify Health");
    CHECK(rig.have(k_ring) == 2);
    CHECK(rig.inventory.count_extra(k_player, k_ring, 0) == 1);
    CHECK(rig.inventory.count_extra(k_player, k_ring, out.new_extra) == 1);
    CHECK(rig.have(k_grand_full) == 0);
    const auto* extra = rig.extras.find(out.new_extra);
    REQUIRE(extra != nullptr);
    CHECK(extra->name == out.name);
    CHECK(extra->enchantment == out.created);
    CHECK(rig.created.is_created(out.created));
    const auto* made = rig.created.find_enchantment(out.created);
    REQUIRE(made != nullptr);
    CHECK(made->constant);
    CHECK(made->effects.at(0).magnitude == Approx(20.0F));

    // The resolver reads it like a world enchantment.
    const auto resolved = magic::resolve_enchantment(*rig.world, &rig.created, out.created);
    REQUIRE(resolved.has_value());
    CHECK(resolved->created);
    CHECK(resolved->constant);
    CHECK(resolved->effects.at(0).effect == k_mg_fortify);
    CHECK(magic::resolve_enchantment(*rig.world, nullptr, out.created) == std::nullopt);
    CHECK(magic::resolve_enchantment(*rig.world, &rig.created, k_en_fire)->world != nullptr);

    // The same enchantment on another ring shares the created form.
    rig.inventory.add(k_player, k_grand_full, 1);
    const auto again = rig.table.enchant(rig.request(k_ring, k_en_fortify, k_grand_full));
    REQUIRE(again.ok());
    CHECK(again.created == out.created);
    CHECK(rig.created.enchantments().size() == 1);
}

TEST_CASE("enchanting a weapon sets its charge, keeps its tempering and takes a given name", "[enchanting][enchant]") {
    Rig rig;
    vm::ItemExtra tempered;
    tempered.health = 1.2F;
    const auto temper_id = rig.extras.intern(tempered);
    rig.inventory.add(k_player, k_sword, 1, temper_id);
    rig.inventory.add(k_player, k_lesser_full, 1);
    auto req = rig.request(k_sword, k_en_fire, k_lesser_full);
    req.item_extra = temper_id;
    req.name = "Flamebrand";
    const auto out = rig.table.enchant(req);
    REQUIRE(out.ok());
    const auto* extra = rig.extras.find(out.new_extra);
    REQUIRE(extra != nullptr);
    CHECK(extra->name == "Flamebrand");
    CHECK(extra->health == Approx(1.2F));
    CHECK(extra->charge_max == Approx(500.0F));
    CHECK(extra->charge == Approx(vm::k_full_charge));
    CHECK(rig.inventory.count_extra(k_player, k_sword, temper_id) == 0);
    const auto info = magic::weapon_charge(*rig.world, k_sword, extra, {}, &rig.created);
    CHECK(info.enchantment == out.created);
    CHECK(info.max == Approx(500.0F));
    CHECK(info.charge == Approx(500.0F));
    CHECK(info.cost == Approx(magic::weapon_use_cost(100.0, 15.0)));
}

TEST_CASE("enchanting refuses what does not work", "[enchanting][enchant]") {
    Rig rig;
    for (const auto form : {k_sword, k_dagger, k_ring, k_found_sword, k_banned_sword, k_staff, k_misc}) {
        rig.inventory.add(k_player, form, 1);
    }
    rig.inventory.add(k_player, k_grand_full, 1);
    rig.inventory.add(k_player, k_grand, 1);
    using R = magic::Refusal;
    CHECK(rig.table.enchant(rig.request(k_sword, k_en_fire, k_grand_full)).ok());
    rig.inventory.add(k_player, k_grand_full, 1);
    CHECK(rig.table.preview(rig.request(k_found_sword, k_en_fire, k_grand_full)).refusal == R::enchanted);
    CHECK(rig.table.preview(rig.request(k_banned_sword, k_en_fire, k_grand_full)).refusal == R::enchanted);
    CHECK(rig.table.preview(rig.request(k_staff, k_en_fire, k_grand_full)).refusal == R::staff);
    CHECK(rig.table.preview(rig.request(k_misc, k_en_fire, k_grand_full)).refusal == R::not_enchantable);
    CHECK(rig.table.preview(rig.request(k_dagger, k_en_sword_only, k_grand_full)).refusal == R::does_not_fit);
    CHECK(rig.table.preview(rig.request(k_dagger, k_en_fortify, k_grand_full)).refusal == R::does_not_fit);
    CHECK(rig.table.preview(rig.request(k_ring, k_en_banned, k_grand_full)).refusal == R::unknown_enchantment);
    CHECK(rig.table.preview(rig.request(k_ring, k_en_fire_big, k_grand_full)).refusal == R::unknown_enchantment);
    CHECK(rig.table.preview(rig.request(k_ring, k_en_fortify, k_grand)).refusal == R::no_gem);       // empty
    CHECK(rig.table.preview(rig.request(k_ring, k_en_fortify, k_petty_full)).refusal == R::no_gem);   // not carried
    CHECK(rig.table.preview(rig.request(k_ring, k_en_fortify, k_misc)).refusal == R::no_gem);
    CHECK(rig.table.preview(rig.request(0x9999, k_en_fortify, k_grand_full)).refusal == R::no_item);
    // A preview changes nothing.
    CHECK(rig.created.enchantments().size() == 1);
    CHECK(rig.have(k_grand_full) == 1);
    // An enchantment with a banned effect cannot be put on once learned either.
    rig.book.learn_enchantment(k_en_banned);
    CHECK(rig.table.preview(rig.request(k_ring, k_en_banned, k_grand_full)).refusal == R::forbidden);
}

TEST_CASE("a reusable soul gem is emptied, not spent", "[enchanting][enchant]") {
    Rig rig;
    rig.inventory.add(k_player, k_ring, 3);
    // Azura's Star keeps its soul in an extra.
    vm::ItemExtra full;
    full.soul = 5;
    const auto full_id = rig.extras.intern(full);
    rig.inventory.add(k_player, k_azura, 1, full_id);
    REQUIRE(rig.table.enchant({.item = k_ring, .item_extra = 0, .enchantment = k_en_fortify, .gem = k_azura, .gem_extra = full_id, .name = {}}).ok());
    CHECK(rig.have(k_azura) == 1);
    CHECK(rig.inventory.count_extra(k_player, k_azura, 0) == 1); // empty again
    CHECK(magic::gem_info(*rig.world, {k_kw_disallow, k_kw_reusable}, k_azura, nullptr).soul == 0);

    // A filled form goes back to the empty form that links to it.
    rig.inventory.add(k_player, k_reuse_full, 1);
    REQUIRE(rig.table.enchant(rig.request(k_ring, k_en_fortify, k_reuse_full)).ok());
    CHECK(rig.have(k_reuse_full) == 0);
    CHECK(rig.have(k_reuse) == 1);
}

// ---- recharging ----------------------------------------------------------------------------------

TEST_CASE("recharging adds the soul's charge up to the weapon's maximum", "[enchanting][recharge]") {
    Rig rig;
    vm::ItemExtra drained;
    drained.enchantment = rig.created.create_enchantment(
        {.id = 0, .base = k_en_fire, .name = "Fire", .effects = {{k_mg_fire, 5.0F, 0, 0}}, .cost = 30.0, .constant = false});
    drained.charge_max = 500.0F;
    drained.charge = 100.0F;
    const auto id = rig.extras.intern(drained);
    rig.inventory.add(k_player, k_sword, 1, id);
    rig.inventory.add(k_player, k_petty_full, 1);
    rig.inventory.add(k_player, k_lesser_full, 2);

    auto out = rig.table.recharge(k_sword, id, k_petty_full, 0);
    REQUIRE(out.ok());
    CHECK(out.charge == Approx(350.0F));
    CHECK(out.max == Approx(500.0F));
    CHECK(rig.have(k_petty_full) == 0);
    CHECK(rig.extras.find(out.new_extra)->charge == Approx(350.0F));
    CHECK(rig.extras.find(out.new_extra)->enchantment == drained.enchantment);
    CHECK(rig.inventory.count_extra(k_player, k_sword, out.new_extra) == 1);
    CHECK(rig.inventory.count_extra(k_player, k_sword, id) == 0);

    out = rig.table.recharge(k_sword, out.new_extra, k_lesser_full, 0);
    REQUIRE(out.ok());
    CHECK(out.charge == Approx(500.0F)); // capped
    out = rig.table.recharge(k_sword, out.new_extra, k_lesser_full, 0);
    CHECK(out.refusal == magic::Refusal::full);
    CHECK(rig.have(k_lesser_full) == 1); // not spent on a full weapon

    rig.inventory.add(k_player, k_dagger, 1);
    CHECK(rig.table.recharge(k_dagger, 0, k_lesser_full, 0).refusal == magic::Refusal::no_enchantment);
    CHECK(rig.table.recharge(k_ring, 0, k_lesser_full, 0).refusal == magic::Refusal::no_item);
}

TEST_CASE("a base enchantment's weapon is recharged from its EAMT", "[enchanting][recharge]") {
    Rig rig;
    vm::ItemExtra used;
    used.charge = 5.0F; // the found sword's ENCH has no charge of its own here: max stays from the extra
    used.charge_max = 100.0F;
    const auto id = rig.extras.intern(used);
    rig.inventory.add(k_player, k_found_sword, 1, id);
    rig.inventory.add(k_player, k_petty_full, 1);
    const auto out = rig.table.recharge(k_found_sword, id, k_petty_full, 0);
    REQUIRE(out.ok());
    CHECK(out.charge == Approx(100.0F));
}

// ---- soul trap -----------------------------------------------------------------------------------

TEST_CASE("a soul fills the smallest empty gem that takes it", "[enchanting][soul]") {
    Rig rig;
    for (const auto form : {k_petty, k_lesser, k_common, k_grand, k_black}) {
        rig.inventory.add(k_player, form, 1);
    }
    auto fill = rig.table.fill_gem(magic::actor_soul(10, false)); // lesser
    REQUIRE(fill.filled);
    CHECK(fill.gem == k_lesser);
    CHECK(fill.form == k_lesser_full); // the empty gem links to its filled form
    CHECK(fill.soul == 2);
    CHECK(rig.have(k_lesser) == 0);
    CHECK(rig.have(k_lesser_full) == 1);

    fill = rig.table.fill_gem(magic::actor_soul(1, false)); // petty: no link, the soul goes on the unit
    REQUIRE(fill.filled);
    CHECK(fill.gem == k_petty);
    CHECK(fill.form == k_petty); // no linked form
    CHECK(fill.extra != 0);
    CHECK(rig.extras.find(fill.extra)->soul == 1);
    CHECK(rig.inventory.count_extra(k_player, k_petty, fill.extra) == 1);
    CHECK(magic::gem_info(*rig.world, {}, k_petty, rig.extras.find(fill.extra)).soul == 1);

    // A filled gem is no longer a candidate: the next petty soul takes the next size up.
    fill = rig.table.fill_gem(magic::actor_soul(1, false));
    REQUIRE(fill.filled);
    CHECK(fill.gem == k_common);

    // A grand creature soul takes the grand gem and leaves the black one.
    fill = rig.table.fill_gem(magic::actor_soul(50, false));
    REQUIRE(fill.filled);
    CHECK(fill.gem == k_grand);
    CHECK(fill.form == k_grand_full);
    // An NPC soul takes only the black gem.
    rig.inventory.add(k_player, k_grand, 1);
    fill = rig.table.fill_gem(magic::actor_soul(50, true));
    REQUIRE(fill.filled);
    CHECK(fill.gem == k_black);
    CHECK(fill.form == k_black_full);
    // Nothing fits any more for a NPC; a creature takes the new grand gem.
    CHECK_FALSE(rig.table.fill_gem(magic::actor_soul(50, true)).filled);
    CHECK(rig.table.fill_gem(magic::actor_soul(50, false)).filled);
    CHECK_FALSE(rig.table.fill_gem({}).filled);
}

TEST_CASE("a soul too big for every carried gem fills none", "[enchanting][soul]") {
    Rig rig;
    rig.inventory.add(k_player, k_petty, 1);
    rig.inventory.add(k_player, k_misc, 1);
    CHECK_FALSE(rig.table.fill_gem(magic::actor_soul(20, false)).filled);
    CHECK(rig.have(k_petty) == 1);
}

// ---- saves ---------------------------------------------------------------------------------------

TEST_CASE("created enchantments round-trip in CRFT 2 and an old section loads without any", "[enchanting][save]") {
    vm::CreatedForms forms;
    vm::CreatedPotion potion;
    potion.name = "Potion";
    potion.effects = {{k_mg_fire, 3.0F, 5, 0}};
    const auto potion_id = forms.create(potion);
    vm::CreatedEnchantment e{.id = 0, .base = k_en_fire, .name = "Fire Damage", .effects = {{k_mg_fire, 7.0F, 3, 12}, {k_mg_fortify, 2.0F, 0, 0}},
                             .cost = 123.5, .constant = false};
    const auto id = forms.create_enchantment(e);
    REQUIRE(id != 0);
    CHECK(forms.create_enchantment(e) == id); // deduplicated by value
    e.cost = 1.0;
    CHECK(forms.create_enchantment(e) != id);
    CHECK(id != potion_id);

    const auto bytes = forms.save();
    vm::CreatedForms back;
    REQUIRE(vm::CreatedForms::load(skydot::save::ByteReader(bytes), back));
    REQUIRE(back.enchantments().size() == 2);
    const auto* got = back.find_enchantment(id);
    REQUIRE(got != nullptr);
    CHECK(got->base == k_en_fire);
    CHECK(got->name == "Fire Damage");
    CHECK(got->cost == Approx(123.5));
    CHECK_FALSE(got->constant);
    REQUIRE(got->effects.size() == 2);
    CHECK(got->effects[0] == vm::CreatedEffect{k_mg_fire, 7.0F, 3, 12});
    CHECK(back.find(potion_id) != nullptr);
    // Ids go on after the loaded ones.
    CHECK(back.create_enchantment({.id = 0, .base = 1, .name = {}, .effects = {{1, 1.0F, 0, 0}}, .cost = 0.0, .constant = false}) > id);

    // Version 1 has no enchantment block: the same bytes without the trailing count.
    vm::CreatedForms plain;
    plain.create(potion);
    auto v1 = plain.save();
    REQUIRE(v1.size() >= 4);
    v1.resize(v1.size() - 4);
    vm::CreatedForms old;
    REQUIRE(vm::CreatedForms::load(skydot::save::ByteReader(v1), old, 1));
    CHECK(old.enchantments().empty());
    CHECK(old.potions().size() == 1);
    // Version 2 bytes are not version 1 bytes.
    CHECK_FALSE(vm::CreatedForms::load(skydot::save::ByteReader(plain.save()), old, 1));
}

TEST_CASE("item extras carry a soul in layout 3 and read layout 2 without it", "[enchanting][save]") {
    vm::ItemExtras table;
    vm::ItemExtra gem;
    gem.soul = 4;
    gem.name = "Gem";
    table.intern(gem);
    vm::ItemExtra other = gem;
    other.soul = 0;
    CHECK(table.intern(other) == 2); // the soul tells extras apart
    skydot::save::ByteWriter w;
    table.write(w);
    vm::ItemExtras back;
    skydot::save::ByteReader r(w.out);
    REQUIRE(vm::ItemExtras::read(r, back));
    REQUIRE(back.size() == 2);
    CHECK(back.find(1)->soul == 4);
    CHECK(back.find(2)->soul == 0);

    skydot::save::ByteWriter v2;
    v2.u32(1);
    v2.f64(1.1);
    v2.u32(0);
    v2.text("Old");
    v2.f64(-1.0);
    v2.f64(0.0);
    v2.u32(0);
    v2.u32(0);
    vm::ItemExtras old;
    skydot::save::ByteReader r2(v2.out);
    REQUIRE(vm::ItemExtras::read(r2, old, 2));
    CHECK(old.find(1)->soul == 0);
    CHECK(old.find(1)->name == "Old");
}
