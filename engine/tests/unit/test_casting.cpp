// SPDX-License-Identifier: GPL-3.0-or-later
//
// Casting (GAMEPLAY-PLAN 7a-3, docs/adr/0008): paying apart from landing, cast_info, the state of a
// hand (HandCast), enchanted weapons (charge, no charge no effect, NPCs never run out), poisons,
// worn enchantments, the item extras' old layout, scrolls in a hand and the actors to derive again
// after a load.
#include "support/world_builder.hpp"

#include "data/fb_search.hpp"
#include "magic/casting.hpp"
#include "magic/enchanted.hpp"
#include "magic/magic_save.hpp"
#include "vm/actor_states.hpp"
#include "vm/created_forms.hpp"
#include "vm/item_extras.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

using namespace skydot::testing;
namespace magic = skydot::magic;
namespace vm = skydot::vm;
using Catch::Approx;

namespace {

constexpr std::uint32_t k_player = 0x14;
constexpr std::uint32_t k_skeever = 0x200;
constexpr std::uint32_t k_health = vm::av::k_health;
constexpr std::uint32_t k_magicka = vm::av::k_magicka;
constexpr std::uint32_t k_destruction = vm::av::k_first_skill + 14;

// MGEF
constexpr std::uint32_t k_fire = 0x1001;     // fire damage per second, concentration
constexpr std::uint32_t k_bolt = 0x1002;     // fire damage once, aimed, with a projectile and an explosion
constexpr std::uint32_t k_heal = 0x1003;     // restore health once, self
constexpr std::uint32_t k_fortify = 0x1004;  // fortify health: peak, recover
constexpr std::uint32_t k_venom = 0x1005;    // damage health per second, hostile (a poison)

// SPEL
constexpr std::uint32_t k_flames = 0x2001;   // concentration, aimed, 14
constexpr std::uint32_t k_firebolt = 0x2002; // fire and forget, aimed, 50, charge 0.5 s
constexpr std::uint32_t k_healing = 0x2003;  // self, 20
constexpr std::uint32_t k_power = 0x2004;    // power, 0 magicka

// Items
constexpr std::uint32_t k_scroll = 0x3001;
constexpr std::uint32_t k_sword = 0x3002;    // enchanted with k_ench_fire, EAMT 100
constexpr std::uint32_t k_plain_sword = 0x3003;
constexpr std::uint32_t k_circlet = 0x3004;  // clothing enchanted with k_ench_fortify
constexpr std::uint32_t k_poison = 0x3005;   // an ALCH of venom
constexpr std::uint32_t k_auto_sword = 0x3006; // enchanted with k_ench_auto

// ENCH
constexpr std::uint32_t k_ench_fire = 0x4001;    // manual cost 25, charge 100
constexpr std::uint32_t k_ench_fortify = 0x4002;
constexpr std::uint32_t k_ench_auto = 0x4003;    // cost summed from the effects

bool fake_source(std::uint32_t ref, vm::ActorBase& out) {
    if (ref != k_player && ref != k_skeever) {
        return false;
    }
    out = vm::ActorBase{};
    out.base[k_health] = 100;
    out.base[k_magicka] = 100;
    out.base[vm::av::k_stamina] = 60;
    out.base[k_destruction] = 15;
    out.is_player = ref == k_player;
    return true;
}

struct Host : magic::MagicHost {
    vm::ActorStates states;
    std::vector<std::string> logs;
    Host() { states.set_source(fake_source); }
    double actor_value(std::uint32_t ref, std::uint32_t av) const override { return states.get(ref, av); }
    void damage_value(std::uint32_t ref, std::uint32_t av, double amount, std::uint32_t) override { states.damage(ref, av, amount); }
    void restore_value(std::uint32_t ref, std::uint32_t av, double amount) override { states.restore(ref, av, amount); }
    void modify_temp(std::uint32_t ref, std::uint32_t av, double delta) override { states.mod_temp(ref, av, delta); }
    bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<magic::wfb::Condition>>*, std::uint32_t,
                         std::uint32_t) override {
        return true;
    }
    bool is_dead(std::uint32_t ref) const override { return states.is_dead(ref); }
    void log(const std::string& line) override { logs.push_back(line); }
};

WorldSpec world_spec() {
    WorldSpec spec;
    spec.format_version = 33;
    const auto mgef = [](std::uint32_t id, std::uint32_t arch, std::int32_t av, std::uint32_t flags, float base_cost) {
        MagicEffectSpec m{.id = id};
        m.archetype = arch;
        m.actor_value = av;
        m.flags = flags;
        m.base_cost = base_cost;
        return m;
    };
    const std::uint32_t hostile = magic::mgef_flag::hostile | magic::mgef_flag::detrimental;
    auto fire = mgef(k_fire, 0, static_cast<std::int32_t>(k_health), hostile, 1.5F);
    fire.casting_type = 2;
    fire.delivery = 2;
    fire.magic_skill = static_cast<std::int32_t>(k_destruction);
    auto bolt = mgef(k_bolt, 0, static_cast<std::int32_t>(k_health), hostile, 5.0F);
    bolt.delivery = 2;
    bolt.magic_skill = static_cast<std::int32_t>(k_destruction);
    bolt.projectile = 0x500;
    bolt.explosion = 0x501;
    bolt.casting_art = 0x502;
    bolt.hit_effect_art = 0x503;
    bolt.casting_light = 0x504;
    auto heal = mgef(k_heal, 0, static_cast<std::int32_t>(k_health), 0, 1.0F);
    auto fortify = mgef(k_fortify, 34, static_cast<std::int32_t>(k_health), magic::mgef_flag::recover, 0.5F);
    auto venom = mgef(k_venom, 0, static_cast<std::int32_t>(k_health), hostile, 2.0F);
    spec.magic_effects = {fire, bolt, heal, fortify, venom};
    spec.spells = {
        SpellSpec{.id = k_flames, .name = "Flames", .cost = 14, .casting_type = 2, .delivery = 2, .range = 1000.0F,
                  .effects = {{.effect = k_fire, .magnitude = 8.0F, .duration = 1}}},
        SpellSpec{.id = k_firebolt, .name = "Firebolt", .cost = 50, .casting_type = 1, .delivery = 2,
                  .charge_time = 0.5F, .cast_duration = 1.0F, .range = 5000.0F,
                  .effects = {{.effect = k_bolt, .magnitude = 25.0F, .area = 10}}},
        SpellSpec{.id = k_healing, .name = "Healing", .cost = 20, .casting_type = 1, .delivery = 0,
                  .effects = {{.effect = k_heal, .magnitude = 10.0F}}},
        SpellSpec{.id = k_power, .name = "Power", .cost = 0, .flags = magic::k_spell_manual_cost, .type = magic::spell_type::power, .casting_type = 1,
                  .delivery = 0, .effects = {{.effect = k_heal, .magnitude = 5.0F}}},
    };
    BaseSpec scroll{.id = k_scroll, .name = "Scroll of Fire"};
    scroll.item = ItemSpec{.category = wfb::ItemCategory::scroll, .effects = {{.effect = k_bolt, .magnitude = 40.0F}}};
    BaseSpec sword{.id = k_sword, .name = "Fire Sword"};
    sword.item = ItemSpec{.category = wfb::ItemCategory::weapon, .weapon_type = 1, .damage = 7.0F,
                          .enchantment = k_ench_fire, .enchantment_amount = 100};
    BaseSpec plain{.id = k_plain_sword, .name = "Iron Sword"};
    plain.item = ItemSpec{.category = wfb::ItemCategory::weapon, .weapon_type = 1, .damage = 7.0F};
    BaseSpec auto_sword{.id = k_auto_sword, .name = "Auto Sword"};
    auto_sword.item = ItemSpec{.category = wfb::ItemCategory::weapon, .weapon_type = 1, .damage = 7.0F,
                               .enchantment = k_ench_auto, .enchantment_amount = 500};
    BaseSpec circlet{.id = k_circlet, .name = "Circlet"};
    circlet.item = ItemSpec{.category = wfb::ItemCategory::clothing, .enchantment = k_ench_fortify};
    BaseSpec poison{.id = k_poison, .name = "Poison"};
    poison.item = ItemSpec{.category = wfb::ItemCategory::potion, .effects = {{.effect = k_venom, .magnitude = 3.0F, .duration = 5}}};
    spec.bases = {scroll, sword, plain, auto_sword, circlet, poison};
    spec.enchantments = {
        EnchantmentSpec{.id = k_ench_fire, .cost = 25, .charge = 100, .flags = magic::k_ench_manual_cost,
                        .effects = {{.effect = k_bolt, .magnitude = 10.0F}}},
        EnchantmentSpec{.id = k_ench_fortify, .cost = 0, .charge = 0, .flags = magic::k_ench_manual_cost,
                        .casting_type = 0, .effects = {{.effect = k_fortify, .magnitude = 30.0F}}},
        EnchantmentSpec{.id = k_ench_auto, .cost = 999, .charge = 500, .flags = 0,
                        .effects = {{.effect = k_bolt, .magnitude = 10.0F}}},
    };
    return spec;
}

struct Rig {
    BuiltWorld world{world_spec()};
    Host host;
    magic::ActiveEffects effects{*world, host};
    magic::SpellBook book{*world, effects};
    vm::ItemExtras extras;

    Rig() {
        book.add_spell(k_player, k_flames);
        book.add_spell(k_player, k_firebolt);
        book.add_spell(k_player, k_power);
    }
    magic::Payment pay(std::uint32_t spell, double seconds = 0.0, double days = 1.0) {
        return magic::pay_spell(*world, host, book, k_player, spell, seconds, days);
    }
    [[nodiscard]] bool has_effect(std::uint32_t target, std::uint32_t mgef) const { return effects.has_effect(target, mgef); }
};

} // namespace

// ---- pay and land ---------------------------------------------------------------------------------

TEST_CASE("paying takes magicka and lands nothing, landing costs nothing", "[casting]") {
    Rig rig;
    const auto paid = rig.pay(k_firebolt);
    REQUIRE(paid.ok);
    CHECK(paid.cost == Approx(50 * magic::skill_cost_multiplier(15)).margin(0.01));
    CHECK(rig.host.states.get(k_player, k_magicka) == Approx(100 - paid.cost));
    CHECK(rig.effects.size() == 0);

    const double before = rig.host.states.get(k_player, k_magicka);
    const auto landed = magic::land_form(*rig.world, rig.effects, k_player, k_firebolt, k_skeever);
    REQUIRE(landed.ok);
    CHECK(landed.instances.size() == 1);
    CHECK(rig.host.states.get(k_player, k_magicka) == Approx(before));
    CHECK(rig.host.states.get(k_skeever, k_health) < 100.0);

    // An area spell is paid once and landed on each actor.
    const auto second = magic::land_form(*rig.world, rig.effects, k_player, k_firebolt, k_player);
    CHECK(second.ok);
    CHECK(rig.host.states.get(k_player, k_magicka) == Approx(before)); // landing again costs nothing either
}

TEST_CASE("paying refuses without magicka and during a power's cooldown", "[casting]") {
    Rig rig;
    rig.host.states.damage(k_player, k_magicka, 95);
    const auto poor = rig.pay(k_firebolt);
    CHECK_FALSE(poor.ok);
    CHECK(poor.reason == "not enough magicka");
    CHECK(rig.host.states.get(k_player, k_magicka) == Approx(5));

    CHECK(rig.pay(k_power, 0.0, 3.0).ok);
    const auto again = rig.pay(k_power, 0.0, 3.5);
    CHECK_FALSE(again.ok);
    CHECK(again.reason == "cooldown");
    CHECK(rig.pay(k_power, 0.0, 4.25).ok);
    CHECK(rig.pay(0x9999).reason == "no such spell");
    CHECK(rig.pay(k_scroll).reason == "no such spell"); // scrolls cost nothing and are not paid
}

TEST_CASE("a concentration spell is paid per second", "[casting]") {
    Rig rig;
    const double per_second = rig.pay(k_flames, 1.0).cost;
    CHECK(per_second == Approx(14 * magic::skill_cost_multiplier(15)).margin(0.01));
    CHECK(rig.pay(k_flames, 0.1).cost == Approx(per_second * 0.1));
    CHECK(rig.pay(k_flames).cost == Approx(per_second)); // 0 seconds: one
}

TEST_CASE("landing a scroll or a spell: target 0 and self delivery are the caster", "[casting]") {
    Rig rig;
    rig.host.states.damage(k_player, k_health, 50);
    const auto heal = magic::land_form(*rig.world, rig.effects, k_player, k_healing, k_skeever);
    REQUIRE(heal.ok);
    CHECK(rig.host.states.get(k_player, k_health) == Approx(60)); // self delivery ignores the target
    CHECK(rig.host.states.get(k_skeever, k_health) == Approx(100));

    const auto scroll = magic::land_form(*rig.world, rig.effects, k_player, k_scroll, 0);
    REQUIRE(scroll.ok);
    CHECK(rig.host.states.get(k_player, k_health) < 60.0); // fire on the caster itself
    CHECK_FALSE(magic::land_form(*rig.world, rig.effects, k_player, 0x9999, 0).ok);
}

// ---- cast_info ------------------------------------------------------------------------------------

TEST_CASE("cast_info says how to cast a spell and a scroll", "[casting]") {
    Rig rig;
    const auto bolt = magic::cast_info(*rig.world, k_firebolt);
    REQUIRE(bolt.found);
    CHECK(bolt.name == "Firebolt");
    CHECK_FALSE(bolt.is_scroll);
    CHECK(bolt.casting_type == magic::casting::fire_and_forget);
    CHECK(bolt.delivery == 2);
    CHECK(bolt.charge_time == Approx(0.5F));
    CHECK(bolt.cast_duration == Approx(1.0F));
    CHECK(bolt.range == Approx(5000.0F));
    CHECK(bolt.mgef == k_bolt);
    CHECK(bolt.projectile == 0x500);
    CHECK(bolt.explosion == 0x501);
    CHECK(bolt.casting_art == 0x502);
    CHECK(bolt.hit_art == 0x503);
    CHECK(bolt.casting_light == 0x504);
    CHECK(bolt.hostile);
    CHECK(bolt.school == static_cast<int>(k_destruction));
    CHECK(bolt.area_feet == 10);
    CHECK(bolt.area_units == Approx(magic::area_units(10)));

    const auto flames = magic::cast_info(*rig.world, k_flames);
    CHECK(flames.casting_type == magic::casting::concentration);

    const auto scroll = magic::cast_info(*rig.world, k_scroll);
    REQUIRE(scroll.found);
    CHECK(scroll.is_scroll);
    CHECK(scroll.name == "Scroll of Fire");
    CHECK(scroll.casting_type == magic::casting::fire_and_forget);
    CHECK(scroll.delivery == 2); // its effect's
    CHECK(scroll.projectile == 0x500);
    CHECK(scroll.charge_time == Approx(0.0F));

    CHECK_FALSE(magic::cast_info(*rig.world, k_sword).found);
    CHECK_FALSE(magic::cast_info(*rig.world, 0x9999).found);
}

// ---- the hand -------------------------------------------------------------------------------------

TEST_CASE("a fire-and-forget hand charges while held and casts on release", "[casting][hand]") {
    magic::HandCast hand;
    const magic::HandCast::Payer never = [](double) { return false; };
    hand.press({magic::casting::fire_and_forget, 0.5});
    CHECK(hand.phase() == magic::HandCast::Phase::charging);
    CHECK(hand.step(0.25, never).kind == magic::HandEvent::Kind::none);
    CHECK(hand.progress() == Approx(0.5));
    const auto full = hand.step(0.3, never);
    CHECK(full.kind == magic::HandEvent::Kind::charged);
    CHECK(hand.charged());
    CHECK(hand.step(0.3, never).kind == magic::HandEvent::Kind::none); // announced once
    const auto cast = hand.release();
    CHECK(cast.kind == magic::HandEvent::Kind::cast);
    CHECK(cast.seconds == Approx(0.85));
    CHECK(hand.phase() == magic::HandCast::Phase::idle);
    CHECK(hand.release().kind == magic::HandEvent::Kind::none);
}

TEST_CASE("releasing before the casting time is up casts nothing", "[casting][hand]") {
    magic::HandCast hand;
    hand.press({magic::casting::fire_and_forget, 0.5});
    hand.step(0.4, {});
    const auto early = hand.release();
    CHECK(early.kind == magic::HandEvent::Kind::none);
    CHECK(std::string(early.name()).empty());
    CHECK(hand.phase() == magic::HandCast::Phase::idle);
    // Charge time 0 (a scroll): the release casts at once.
    hand.press({magic::casting::fire_and_forget, 0.0});
    CHECK(hand.release().kind == magic::HandEvent::Kind::cast);
}

TEST_CASE("a concentration hand pays and ticks until released or out of magicka", "[casting][hand]") {
    magic::HandCast hand;
    double paid = 0.0;
    double budget = 0.25; // seconds the caster can afford
    const magic::HandCast::Payer payer = [&](double seconds) {
        if (budget < seconds) {
            return false;
        }
        budget -= seconds;
        paid += seconds;
        return true;
    };
    hand.press({magic::casting::concentration, 0.0});
    const auto t1 = hand.step(0.1, payer);
    CHECK(t1.kind == magic::HandEvent::Kind::tick);
    CHECK(t1.seconds == Approx(0.1));
    CHECK(hand.step(0.1, payer).kind == magic::HandEvent::Kind::tick);
    const auto out = hand.step(0.1, payer); // 0.05 s left: cannot pay 0.1
    CHECK(out.kind == magic::HandEvent::Kind::stop);
    CHECK(paid == Approx(0.2));
    CHECK(hand.phase() == magic::HandCast::Phase::idle);
    CHECK(hand.release().kind == magic::HandEvent::Kind::none); // already over

    budget = 10.0;
    hand.press({magic::casting::concentration, 0.0});
    hand.step(0.5, payer);
    const auto released = hand.release();
    CHECK(released.kind == magic::HandEvent::Kind::stop);
    CHECK(std::string(released.name()) == "stop");
    CHECK(hand.step(0.5, payer).kind == magic::HandEvent::Kind::none); // idle does not pay
}

// ---- enchanted weapons ------------------------------------------------------------------------------

TEST_CASE("charge math: full sentinel, spending, last charge, NPCs", "[casting][enchant]") {
    CHECK(magic::charge_max(0.0F, 100, 50) == Approx(100.0F));
    CHECK(magic::charge_max(0.0F, 0, 50) == Approx(50.0F));
    CHECK(magic::charge_max(300.0F, 100, 50) == Approx(300.0F)); // a created enchantment's own
    CHECK(magic::charge_now(vm::k_full_charge, 100.0F) == Approx(100.0F));
    CHECK(magic::charge_now(40.0F, 100.0F) == Approx(40.0F));

    auto spent = magic::spend_charge(vm::k_full_charge, 100.0F, 25.0, true);
    CHECK(spent.effect);
    CHECK(spent.after == Approx(75.0F));
    spent = magic::spend_charge(10.0F, 100.0F, 25.0, true);
    CHECK(spent.effect); // the last charge still fires
    CHECK(spent.after == Approx(0.0F));
    spent = magic::spend_charge(0.0F, 100.0F, 25.0, true);
    CHECK_FALSE(spent.effect);
    spent = magic::spend_charge(0.0F, 100.0F, 25.0, false); // an NPC's weapon never runs out
    CHECK(spent.effect);
    CHECK(spent.after == Approx(0.0F));
}

TEST_CASE("an enchantment costs its stored cost, or the sum of its effects", "[casting][enchant]") {
    Rig rig;
    const auto* manual = skydot::lookup(rig.world->enchantments(), k_ench_fire);
    const auto* automatic = skydot::lookup(rig.world->enchantments(), k_ench_auto);
    REQUIRE((manual != nullptr && automatic != nullptr));
    CHECK(magic::enchantment_cost(*rig.world, *manual) == Approx(25.0));
    CHECK(magic::enchantment_cost(*rig.world, *automatic) == Approx(5.0 * std::pow(10.0, 1.1)).margin(0.01));
}

TEST_CASE("a hit with an enchanted weapon fires it and spends the player's charge", "[casting][enchant]") {
    Rig rig;
    magic::HitMagicInput in{.attacker = k_player, .target = k_skeever, .weapon = k_sword, .extra = 0,
                            .attacker_is_player = true};
    auto first = magic::hit_magic(*rig.world, rig.effects, rig.extras, in, {});
    CHECK(first.enchantment == k_ench_fire);
    CHECK(first.fired);
    CHECK(first.max == Approx(100.0F));
    CHECK(first.charge == Approx(75.0F));
    CHECK(first.new_extra != 0);
    CHECK(first.instances.size() == 1);
    CHECK(rig.host.states.get(k_skeever, k_health) < 100.0);
    CHECK(rig.extras.find(first.new_extra)->charge == Approx(75.0F));

    // The charge goes on from the new extra; the weapon info shows it.
    in.extra = first.new_extra;
    const auto info = magic::weapon_charge(*rig.world, k_sword, rig.extras.find(in.extra));
    CHECK(info.charge == Approx(75.0F));
    CHECK(info.cost == Approx(25.0));
    for (int i = 0; i < 3; ++i) {
        const auto r = magic::hit_magic(*rig.world, rig.effects, rig.extras, in, {});
        in.extra = r.new_extra;
    }
    const double health = rig.host.states.get(k_skeever, k_health);
    const auto empty = magic::hit_magic(*rig.world, rig.effects, rig.extras, in, {});
    CHECK_FALSE(empty.fired); // no charge left: no effect
    CHECK(empty.charge == Approx(0.0F));
    CHECK(empty.instances.empty());
    CHECK(rig.host.states.get(k_skeever, k_health) == Approx(health));
    CHECK(empty.new_extra == in.extra);
}

TEST_CASE("a sole holder's extra changes in place; shared ones split off", "[casting][enchant]") {
    Rig rig;
    vm::ItemExtra big;
    big.charge_max = 5000.0F;
    big.charge = 5000.0F;
    std::uint32_t id = rig.extras.intern(big);
    const std::uint32_t first = id;
    float last = 5000.0F;
    for (int i = 0; i < 50; ++i) {
        magic::HitMagicInput in{.attacker = k_player, .target = k_skeever, .weapon = k_sword, .extra = id,
                                .attacker_is_player = true, .extra_unique = true};
        const auto r = magic::hit_magic(*rig.world, rig.effects, rig.extras, in, {});
        REQUIRE(r.fired);
        id = r.new_extra;
        CHECK(id == first);
        const float now = rig.extras.find(id)->charge;
        CHECK(now < last);
        last = now;
    }
    CHECK(rig.extras.size() == 1);

    // Two units share the id: the one hit gets its own, the other keeps the old.
    magic::HitMagicInput shared{.attacker = k_player, .target = k_skeever, .weapon = k_sword, .extra = first,
                                .attacker_is_player = true, .extra_unique = false};
    const auto r = magic::hit_magic(*rig.world, rig.effects, rig.extras, shared, {});
    CHECK(r.new_extra != first);
    CHECK(rig.extras.size() == 2);
    CHECK(rig.extras.find(first)->charge == Approx(last));
}

TEST_CASE("an NPC's enchanted weapon never runs out and keeps its extra", "[casting][enchant]") {
    Rig rig;
    const magic::HitMagicInput in{.attacker = k_skeever, .target = k_player, .weapon = k_sword, .extra = 0,
                                  .attacker_is_player = false};
    for (int i = 0; i < 10; ++i) {
        const auto r = magic::hit_magic(*rig.world, rig.effects, rig.extras, in, {});
        REQUIRE(r.fired);
        CHECK(r.new_extra == 0);
    }
    CHECK(rig.extras.size() == 0);
    CHECK(rig.host.states.get(k_player, k_health) < 100.0);
}

TEST_CASE("an unenchanted weapon does nothing; a created enchantment's own maximum counts", "[casting][enchant]") {
    Rig rig;
    const magic::HitMagicInput in{.attacker = k_player, .target = k_skeever, .weapon = k_plain_sword,
                                  .attacker_is_player = true};
    const auto none = magic::hit_magic(*rig.world, rig.effects, rig.extras, in, {});
    CHECK(none.enchantment == 0);
    CHECK_FALSE(none.fired);
    CHECK(none.new_extra == 0);

    vm::ItemExtra made;
    made.enchantment = k_ench_fire;
    made.charge_max = 400.0F;
    const auto id = rig.extras.intern(made);
    const auto info = magic::weapon_charge(*rig.world, k_plain_sword, rig.extras.find(id));
    CHECK(info.enchantment == k_ench_fire);
    CHECK(info.max == Approx(400.0F));
    CHECK(info.charge == Approx(400.0F));
}

// ---- poisons -----------------------------------------------------------------------------------------

TEST_CASE("a poison on a weapon is delivered by one hit and then cleared", "[casting][poison]") {
    Rig rig;
    const magic::PoisonInputs inputs = [](std::uint32_t poison, std::vector<magic::EffectInput>& out) {
        if (poison != k_poison) {
            return false;
        }
        out.push_back({k_venom, 3.0F, 0, 5, 0});
        return true;
    };
    vm::ItemExtra bare;
    CHECK(magic::can_poison(bare));
    const auto poisoned = magic::poisoned(bare, k_poison, 1);
    CHECK_FALSE(magic::can_poison(poisoned));
    const std::uint32_t extra = rig.extras.intern(poisoned);
    REQUIRE(extra != 0);

    const magic::HitMagicInput in{.attacker = k_player, .target = k_skeever, .weapon = k_plain_sword, .extra = extra,
                                  .attacker_is_player = true};
    const auto hit = magic::hit_magic(*rig.world, rig.effects, rig.extras, in, inputs);
    CHECK(hit.poison == k_poison);
    CHECK(hit.instances.size() == 1);
    CHECK(rig.has_effect(k_skeever, k_venom));
    CHECK(hit.new_extra == 0); // the poison is gone: the unit is plain again
    const auto source = rig.effects.instance(hit.instances.front())->source;
    CHECK(source.kind == magic::SourceKind::poison);
    CHECK(source.form == k_poison);

    // The next hit with the (now plain) weapon delivers nothing.
    rig.effects.dispel_all(k_skeever);
    const magic::HitMagicInput next{.attacker = k_player, .target = k_skeever, .weapon = k_plain_sword,
                                    .extra = hit.new_extra, .attacker_is_player = true};
    const auto again = magic::hit_magic(*rig.world, rig.effects, rig.extras, next, inputs);
    CHECK(again.poison == 0);
    CHECK_FALSE(rig.has_effect(k_skeever, k_venom));
}

TEST_CASE("a poisoned enchanted weapon keeps its charge when the poison goes", "[casting][poison]") {
    Rig rig;
    const magic::PoisonInputs inputs = [](std::uint32_t, std::vector<magic::EffectInput>& out) {
        out.push_back({k_venom, 3.0F, 0, 5, 0});
        return true;
    };
    vm::ItemExtra e;
    e.charge = 60.0F;
    const std::uint32_t extra = rig.extras.intern(magic::poisoned(e, k_poison, 2));
    magic::HitMagicInput in{.attacker = k_player, .target = k_skeever, .weapon = k_sword, .extra = extra,
                            .attacker_is_player = true};
    const auto first = magic::hit_magic(*rig.world, rig.effects, rig.extras, in, inputs);
    const auto* now = rig.extras.find(first.new_extra);
    REQUIRE(now != nullptr);
    CHECK(now->poison == k_poison);
    CHECK(now->poison_doses == 1);
    CHECK(now->charge == Approx(35.0F));
}

// ---- worn enchantments ---------------------------------------------------------------------------------

TEST_CASE("worn enchantments start on equip, end on removal and are never saved", "[casting][enchant][worn]") {
    Rig rig;
    magic::WornEnchantments worn(*rig.world, rig.effects);
    const double max = rig.host.states.max(k_player, k_health);
    worn.sync(k_player, {{k_circlet, 0, k_ench_fortify}});
    CHECK(rig.has_effect(k_player, k_fortify));
    CHECK(rig.host.states.max(k_player, k_health) == Approx(max + 30));
    const auto view = rig.effects.effects_on(k_player).front();
    CHECK(view.source.kind == magic::SourceKind::enchantment);
    CHECK(view.source.form == k_circlet);
    CHECK(view.permanent);

    worn.sync(k_player, {{k_circlet, 0, k_ench_fortify}}); // nothing changes
    CHECK(rig.effects.size() == 1);

    // Not saved: a load derives them again.
    const auto bytes = rig.effects.save();
    magic::ActiveEffects loaded(*rig.world, rig.host);
    REQUIRE(loaded.load(skydot::save::ByteReader(bytes)));
    CHECK(loaded.size() == 0);

    worn.sync(k_player, {});
    CHECK_FALSE(rig.has_effect(k_player, k_fortify));
    CHECK(rig.host.states.max(k_player, k_health) == Approx(max));
    CHECK(worn.applied(k_player).empty());

    // Death ends them with everything else; the state is forgotten, a new sync starts them again.
    worn.sync(k_player, {{k_circlet, 0, k_ench_fortify}});
    rig.effects.on_death(k_player);
    CHECK_FALSE(rig.has_effect(k_player, k_fortify));
    worn.forget(k_player);
    worn.sync(k_player, {{k_circlet, 0, k_ench_fortify}});
    CHECK(rig.has_effect(k_player, k_fortify));
}

// ---- saves -----------------------------------------------------------------------------------------------

TEST_CASE("item extras of the old layout load with the new fields at their defaults", "[casting][save]") {
    skydot::save::ByteWriter w;
    w.u32(2);
    w.f64(1.2);
    w.u32(k_ench_fire);
    w.text("Old Blade");
    w.f64(1.5);
    w.u32(0);
    w.text("");
    vm::ItemExtras old;
    REQUIRE(vm::ItemExtras::read(*std::make_unique<skydot::save::ByteReader>(w.out), old, 1));
    REQUIRE(old.size() == 2);
    const auto* first = old.find(1);
    REQUIRE(first != nullptr);
    CHECK(first->health == Approx(1.2F));
    CHECK(first->enchantment == k_ench_fire);
    CHECK(first->name == "Old Blade");
    CHECK(first->charge == Approx(vm::k_full_charge));
    CHECK(first->charge_max == Approx(0.0F));
    CHECK(first->poison == 0);
    CHECK(first->poison_doses == 0);

    // The new layout round-trips.
    vm::ItemExtras fresh;
    vm::ItemExtra e;
    e.enchantment = k_ench_fire;
    e.charge = 12.5F;
    e.charge_max = 300.0F;
    e.poison = k_poison;
    e.poison_doses = 3;
    fresh.intern(e);
    skydot::save::ByteWriter out;
    fresh.write(out);
    vm::ItemExtras back;
    skydot::save::ByteReader r(out.out);
    REQUIRE(vm::ItemExtras::read(r, back));
    REQUIRE(back.size() == 1);
    CHECK(back.find(1)->same(e));
}

TEST_CASE("a scroll can be held in a hand and survives MAGC", "[casting][save]") {
    Rig rig;
    CHECK_FALSE(rig.book.equip(k_player, k_scroll, magic::Slot::left));       // not a known spell
    CHECK_FALSE(rig.book.equip(k_player, k_scroll, magic::Slot::power, true)); // never the power slot
    REQUIRE(rig.book.equip(k_player, k_scroll, magic::Slot::left, true));
    rig.book.equip(k_player, k_firebolt, magic::Slot::right);
    CHECK(rig.book.equipped(k_player, magic::Slot::left) == k_scroll);

    const auto bytes = magic::write_magc(rig.effects, rig.book, magic::ShoutBook(*rig.world));
    magic::MagcParts parts;
    REQUIRE(magic::read_magc(skydot::save::ByteReader(bytes), parts));
    REQUIRE(magic::check_magc(*rig.world, parts));
    Rig other;
    REQUIRE(magic::SpellBook::load(skydot::save::ByteReader(parts.book), other.book));
    CHECK(other.book.equipped(k_player, magic::Slot::left) == k_scroll);
    CHECK(other.book.equipped(k_player, magic::Slot::right) == k_firebolt);
}

// ---- after a load ------------------------------------------------------------------------------------------

TEST_CASE("actors_to_rederive: the player, the book's actors, effect targets and wearers", "[casting][save]") {
    Rig rig; // the book has records for the player
    constexpr std::uint32_t a = 0x300, b = 0x301, c = 0x302, d = 0x303, e = 0x304;
    REQUIRE(rig.book.add_spell(a, k_healing));
    REQUIRE(rig.book.add_spell(b, k_flames));
    REQUIRE(rig.book.add_spell(b, k_firebolt));
    REQUIRE(rig.book.add_spell(c, k_power));
    const magic::EffectInput in{k_heal, 5.0F, 0, 30, -1};
    rig.effects.apply(a, d, {magic::SourceKind::spell, k_healing, 0}, std::span(&in, 1));
    rig.effects.apply(a, e, {magic::SourceKind::spell, k_healing, 0}, std::span(&in, 1));

    const auto all = magic::actors_to_rederive(rig.book, rig.effects);
    CHECK(all == std::set<std::uint32_t>{k_player, a, b, c, d, e});

    const auto with_wearers = magic::actors_to_rederive(rig.book, rig.effects, {0x400, a});
    CHECK(with_wearers.size() == 7);
    CHECK(with_wearers.contains(0x400));

    // A bare world still re-derives the player.
    Rig bare;
    bare.book.clear();
    CHECK(magic::actors_to_rederive(bare.book, bare.effects) == std::set<std::uint32_t>{k_player});
}

// ---- created enchantments ------------------------------------------------------------------------------

TEST_CASE("a created weapon enchantment fires its effects and spends its own cost", "[casting][enchant][created]") {
    Rig rig;
    vm::CreatedForms created;
    const auto made = created.create_enchantment({.id = 0, .base = k_ench_fire, .name = "Fire", .effects = {{k_bolt, 12.0F, 0, 0}},
                                                  .cost = 40.0, .constant = false});
    vm::ItemExtra extra;
    extra.enchantment = made;
    extra.charge_max = 200.0F;
    const auto id = rig.extras.intern(extra);

    const auto info = magic::weapon_charge(*rig.world, k_plain_sword, rig.extras.find(id), {}, &created);
    CHECK(info.enchantment == made);
    CHECK(info.max == Approx(200.0F));
    CHECK(info.charge == Approx(200.0F));
    CHECK(info.cost == Approx(40.0));
    // Without the table of made enchantments it is nothing.
    CHECK(magic::weapon_charge(*rig.world, k_plain_sword, rig.extras.find(id)).enchantment == 0);

    magic::HitMagicInput in{.attacker = k_player, .target = k_skeever, .weapon = k_plain_sword, .extra = id,
                            .attacker_is_player = true};
    const auto hit = magic::hit_magic(*rig.world, rig.effects, rig.extras, in, {}, {}, &created);
    CHECK(hit.enchantment == made);
    CHECK(hit.fired);
    CHECK(hit.charge == Approx(160.0F));
    CHECK(hit.instances.size() == 1);
    CHECK(rig.host.states.get(k_skeever, k_health) < 100.0);
    CHECK(rig.extras.find(hit.new_extra)->charge == Approx(160.0F));
}

TEST_CASE("a created apparel enchantment is worn like a world one", "[casting][enchant][worn][created]") {
    Rig rig;
    vm::CreatedForms created;
    const auto made = created.create_enchantment({.id = 0, .base = k_ench_fortify, .name = "Fortify", .effects = {{k_fortify, 12.0F, 0, 0}},
                                                  .cost = 0.0, .constant = true});
    magic::WornEnchantments worn(*rig.world, rig.effects, &created);
    const double max = rig.host.states.max(k_player, k_health);
    worn.sync(k_player, {{k_circlet, 1, made}});
    CHECK(rig.has_effect(k_player, k_fortify));
    CHECK(rig.host.states.max(k_player, k_health) == Approx(max + 12));
    worn.sync(k_player, {});
    CHECK(rig.host.states.max(k_player, k_health) == Approx(max));
}
