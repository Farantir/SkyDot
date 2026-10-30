// SPDX-License-Identifier: GPL-3.0-or-later
//
// ActorStates (vm/actor_states.hpp): bases from a fake source, every op,
// regeneration with its delay, death and the ACTV section; and the Actor
// natives over it.
#include "support/vm_rig.hpp"

#include "vm/actor_states.hpp"
#include "vm/natives_actor.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace skydot::testing;
using namespace skydot::vm;
using Catch::Approx;

namespace {

constexpr std::uint32_t k_hero = 0x14;    // the player
constexpr std::uint32_t k_guard = 0x100;  // an ordinary NPC
constexpr std::uint32_t k_elder = 0x101;  // essential
constexpr std::uint32_t k_rock = 0x200;   // no actor

bool fake_source(std::uint32_t ref, ActorBase& out) {
    if (ref != k_hero && ref != k_guard && ref != k_elder) {
        return false;
    }
    out = ActorBase{};
    out.base[av::k_health] = 100;
    out.base[av::k_magicka] = 80;
    out.base[av::k_stamina] = 60;
    out.base[av::k_heal_rate] = 1.0;    // percent of the maximum per second
    out.base[av::k_magicka_rate] = 2.0;
    out.base[av::k_stamina_rate] = 5.0;
    out.base[av::k_heal_rate_mult] = 100;
    out.base[av::k_magicka_rate_mult] = 100;
    out.base[av::k_stamina_rate_mult] = 100;
    out.base[av::k_first_skill] = 20; // OneHanded
    out.is_player = ref == k_hero;
    out.essential = ref == k_elder;
    out.npc = ref + 0x1000; // the NPC_ a reference is
    return true;
}

ActorStates make() {
    ActorStates s;
    s.set_source(fake_source);
    return s;
}

} // namespace

TEST_CASE("actor value names are found in any case, with the index table's anchors", "[actor]") {
    CHECK(av::index_of("health") == 24u);
    CHECK(av::index_of("MAGICKA") == 25u);
    CHECK(av::index_of("Stamina") == 26u);
    CHECK(av::index_of("OneHanded") == 6u);
    CHECK(av::index_of("Enchanting") == 23u);
    CHECK(av::index_of("CarryWeight") == 32u);
    CHECK(av::index_of("ReflectDamage") == 163u);
    CHECK(av::index_of("Archery") == 8u);
    CHECK_FALSE(av::index_of("NoSuchValue").has_value());
    CHECK(av::k_names[av::k_speed_mult] == "SpeedMult");
}

TEST_CASE("values come from the base, a non-actor reads 0", "[actor]") {
    auto s = make();
    CHECK(s.get(k_guard, av::k_health) == Approx(100));
    CHECK(s.max(k_guard, av::k_magicka) == Approx(80));
    CHECK(s.get(k_guard, av::k_first_skill) == Approx(20));
    CHECK(s.get(k_guard, av::k_carry_weight) == Approx(0));
    CHECK(s.is_actor(k_guard));
    CHECK_FALSE(s.is_actor(k_rock));
    CHECK(s.get(k_rock, av::k_health) == 0.0);
    s.damage(k_rock, av::k_health, 5);
    s.set_base(k_rock, av::k_health, 5);
    CHECK(s.get(k_rock, av::k_health) == 0.0);
    CHECK(s.get(k_guard, 9999) == 0.0);
}

TEST_CASE("set, mod, force, damage and restore", "[actor]") {
    auto s = make();
    s.set_base(k_guard, av::k_stamina, 70);
    CHECK(s.get_base(k_guard, av::k_stamina) == Approx(70));
    CHECK(s.get(k_guard, av::k_stamina) == Approx(70));
    s.mod(k_guard, av::k_stamina, 30);
    CHECK(s.get_base(k_guard, av::k_stamina) == Approx(70));
    CHECK(s.max(k_guard, av::k_stamina) == Approx(100));
    s.damage(k_guard, av::k_stamina, 40);
    CHECK(s.get(k_guard, av::k_stamina) == Approx(60));
    CHECK(s.max(k_guard, av::k_stamina) == Approx(100));
    CHECK(s.percentage(k_guard, av::k_stamina) == Approx(0.6));
    s.restore(k_guard, av::k_stamina, 15);
    CHECK(s.get(k_guard, av::k_stamina) == Approx(75));
    s.restore(k_guard, av::k_stamina, 1000); // up to no damage, not beyond
    CHECK(s.get(k_guard, av::k_stamina) == Approx(100));
    s.damage(k_guard, av::k_stamina, 10);
    s.force(k_guard, av::k_stamina, 33);
    CHECK(s.get(k_guard, av::k_stamina) == Approx(33));
    s.damage(k_guard, av::k_magicka, 1000); // stops at 0
    CHECK(s.get(k_guard, av::k_magicka) == Approx(0));
    CHECK(s.percentage(k_guard, av::k_magicka) == Approx(0));
    s.damage(k_guard, av::k_magicka, -5); // not a damage
    CHECK(s.get(k_guard, av::k_magicka) == Approx(0));
}

TEST_CASE("percentage is 1 when there is no maximum", "[actor]") {
    auto s = make();
    CHECK(s.percentage(k_guard, av::k_carry_weight) == Approx(1.0));
    s.set_base(k_guard, av::k_carry_weight, -3);
    CHECK(s.percentage(k_guard, av::k_carry_weight) == Approx(1.0));
}

TEST_CASE("the regeneration delays default to the GMST values", "[actor]") {
    CHECK(ActorStates::k_damaged_magicka_regen_delay == 0.5);
    CHECK(ActorStates::k_damaged_stamina_regen_delay == 0.5);
}

TEST_CASE("damaged values regenerate per second by the rate, Magicka and Stamina after a delay", "[actor]") {
    auto s = make();
    s.damage(k_guard, av::k_health, 50);
    s.tick(10); // 1% of 100 per second
    CHECK(s.get(k_guard, av::k_health) == Approx(60));
    s.tick(1000);
    CHECK(s.get(k_guard, av::k_health) == Approx(100)); // not beyond the maximum

    s.set_regen_delays(1.5, 1.5);
    s.damage(k_guard, av::k_stamina, 30);
    s.damage(k_guard, av::k_magicka, 40);
    s.tick(1.0);
    CHECK(s.get(k_guard, av::k_stamina) == Approx(30));
    CHECK(s.get(k_guard, av::k_magicka) == Approx(40));
    s.tick(1.0); // the delay ends half way: 0.5 s regenerate (5% of 60 = 3 per second)
    s.tick(2.0);
    CHECK(s.get(k_guard, av::k_stamina) == Approx(30 + 7.5));
    CHECK(s.get(k_guard, av::k_magicka) == Approx(40 + 2.5 * 2.0 * 0.8));

    // The rate multiplier scales it.
    s.set_base(k_guard, av::k_stamina_rate_mult, 200);
    s.tick(1.0);
    CHECK(s.get(k_guard, av::k_stamina) == Approx(30 + 7.5 + 6));
    // Damage again starts a new delay.
    s.damage(k_guard, av::k_stamina, 1);
    s.tick(1.0);
    CHECK(s.get(k_guard, av::k_stamina) == Approx(30 + 7.5 + 6 - 1));
}

TEST_CASE("an actor at Health 0 dies once, unless essential or the player", "[actor]") {
    auto s = make();
    s.damage(k_guard, av::k_health, 99);
    CHECK_FALSE(s.is_dead(k_guard));
    CHECK(s.take_deaths().empty());
    s.damage(k_guard, av::k_health, 5);
    CHECK(s.is_dead(k_guard));
    CHECK(s.get(k_guard, av::k_health) == Approx(0));
    CHECK(s.take_deaths() == std::vector<std::uint32_t>{k_guard});
    CHECK(s.take_deaths().empty());
    s.tick(60); // the dead do not regenerate
    CHECK(s.get(k_guard, av::k_health) == Approx(0));
    s.damage(k_guard, av::k_health, 5);
    CHECK(s.take_deaths().empty());

    for (const auto ref : {k_elder, k_hero}) {
        s.damage(ref, av::k_health, 500);
        CHECK_FALSE(s.is_dead(ref));
        CHECK(s.get(ref, av::k_health) == Approx(0));
        s.tick(10);
        CHECK(s.get(ref, av::k_health) == Approx(10)); // they regenerate
    }
    CHECK(s.take_deaths().empty());

    // A script setting Health to 0 kills as well.
    auto t = make();
    t.set_base(k_guard, av::k_health, 0);
    CHECK(t.is_dead(k_guard));
}

TEST_CASE("forget and clear drop an actor's changes", "[actor]") {
    auto s = make();
    s.damage(k_guard, av::k_health, 10);
    s.damage(k_elder, av::k_health, 10);
    s.forget(k_guard);
    CHECK(s.get(k_guard, av::k_health) == Approx(100));
    CHECK(s.get(k_elder, av::k_health) == Approx(90));
    s.clear();
    CHECK(s.get(k_elder, av::k_health) == Approx(100));
}

TEST_CASE("ACTV round-trips and refuses damaged sections", "[actor][save]") {
    auto s = make();
    s.damage(k_guard, av::k_health, 100);
    s.set_base(k_elder, av::k_stamina, 12.5);
    s.mod(k_elder, av::k_stamina, 3);
    s.damage(k_elder, av::k_stamina, 4);
    const auto bytes = s.save();

    ActorStates loaded;
    REQUIRE(ActorStates::load(skydot::save::ByteReader(bytes), loaded));
    ActorStates t = make();
    t.adopt(std::move(loaded));
    CHECK(t.is_dead(k_guard));
    CHECK(t.get_base(k_elder, av::k_stamina) == Approx(12.5));
    CHECK(t.max(k_elder, av::k_stamina) == Approx(15.5));
    CHECK(t.get(k_elder, av::k_stamina) == Approx(11.5));
    CHECK(t.get(k_elder, av::k_health) == Approx(100)); // untouched: from the data

    ActorStates scratch;
    for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
        const std::vector<std::uint8_t> shorter(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(ActorStates::load(skydot::save::ByteReader(shorter), scratch));
    }
    auto longer = bytes;
    longer.push_back(0);
    CHECK_FALSE(ActorStates::load(skydot::save::ByteReader(longer), scratch));
    CHECK(ActorStates::load(skydot::save::ByteReader(ActorStates{}.save()), scratch));
}

TEST_CASE("ACTV 2 keeps the killer and the corpse place, version 1 still loads", "[actor][save]") {
    auto s = make();
    s.damage(k_guard, av::k_health, 100);
    s.set_killer(k_guard, k_hero);
    s.set_corpse_place(k_guard, {.space = 0x3C, .x = 1.5, .y = -2.0, .z = 300.0, .rot_z = 0.75});
    CHECK(s.killer_of(k_guard) == k_hero);
    CHECK(s.killer_of(k_elder) == 0);
    const auto bytes = s.save();
    ActorStates loaded;
    REQUIRE(ActorStates::load(skydot::save::ByteReader(bytes), loaded));
    ActorStates t = make();
    t.adopt(std::move(loaded));
    CHECK(t.is_dead(k_guard));
    CHECK(t.killer_of(k_guard) == k_hero);
    const auto place = t.corpse_place(k_guard);
    REQUIRE(place.has_value());
    CHECK(place->space == 0x3C);
    CHECK(place->y == Approx(-2.0));
    CHECK(place->rot_z == Approx(0.75));
    CHECK_FALSE(t.corpse_place(k_elder).has_value());
    ActorStates scratch;
    for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
        const std::vector<std::uint8_t> shorter(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(ActorStates::load(skydot::save::ByteReader(shorter), scratch));
    }

    // Version 1: one dead actor, one value (Health base 7), nothing after.
    skydot::save::ByteWriter w;
    w.u32(1);
    w.u32(k_guard);
    w.u8(1);
    w.u32(1);
    w.u32(av::k_health);
    w.u8(1);
    w.f64(7.0);
    w.f64(0.0);
    w.f64(-7.0);
    ActorStates old;
    REQUIRE(ActorStates::load(skydot::save::ByteReader(w.out), old, 1));
    ActorStates u = make();
    u.adopt(std::move(old));
    CHECK(u.is_dead(k_guard));
    CHECK(u.killer_of(k_guard) == 0);
    CHECK_FALSE(u.corpse_place(k_guard).has_value());
}

TEST_CASE("the Actor natives read and change values; non-actors and unknown names read 0 and report once",
          "[actor][natives]") {
    auto states = make();
    Rig rig;
    rig.add({.name = "Actor",
             .functions = {native_function("GetActorValue", {{"av", "String"}}, "Float", false),
                           native_function("DamageActorValue", {{"av", "String"}, {"v", "Float"}}, "None", false),
                           native_function("GetActorValuePercentage", {{"av", "String"}}, "Float", false),
                           native_function("IsDead", {}, "Bool", false)}});
    bind_actor_natives(rig.vm, states);

    auto hurt = global_function("hurt", {{"a", "Actor"}, {"n", "String"}, {"v", "Float"}});
    hurt.add(Op::callmethod, {ident("DamageActorValue"), ident("a"), ident("::NoneVar"), ident("n"), ident("v")})
        .add(Op::return_, {ident("::NoneVar")});
    auto value = global_function("value", {{"a", "Actor"}, {"n", "String"}}, {{"r", "Float"}}, "Float");
    value.add(Op::callmethod, {ident("GetActorValue"), ident("a"), ident("r"), ident("n")})
        .add(Op::return_, {ident("r")});
    auto percent = global_function("percent", {{"a", "Actor"}, {"n", "String"}}, {{"r", "Float"}}, "Float");
    percent.add(Op::callmethod, {ident("GetActorValuePercentage"), ident("a"), ident("r"), ident("n")})
        .add(Op::return_, {ident("r")});
    auto dead = global_function("dead", {{"a", "Actor"}}, {{"r", "Bool"}}, "Bool");
    dead.add(Op::callmethod, {ident("IsDead"), ident("a"), ident("r")}).add(Op::return_, {ident("r")});
    rig.add(script("Caller", {hurt, value, percent, dead}));

    const auto guard = rig.vm.object(k_guard, "Actor");
    CHECK(rig.call("caller", "value", {guard, String("health")}).f() == Approx(100));
    rig.call("caller", "hurt", {guard, String("Health"), Float(25)});
    CHECK(rig.call("caller", "value", {guard, String("HEALTH")}).f() == Approx(75));
    CHECK(rig.call("caller", "percent", {guard, String("Health")}).f() == Approx(0.75));
    CHECK_FALSE(rig.call("caller", "dead", {guard}).b());
    rig.call("caller", "hurt", {guard, String("Health"), Float(80)});
    CHECK(rig.call("caller", "dead", {guard}).b());
    CHECK(rig.vm.errors() == 0);

    CHECK(rig.call("caller", "value", {guard, String("Bogus")}).f() == 0.0F);
    CHECK(rig.call("caller", "value", {guard, String("Bogus")}).f() == 0.0F);
    CHECK(rig.vm.errors() == 1); // reported once
    const auto rock = rig.vm.object(k_rock, "Actor");
    CHECK(rig.call("caller", "value", {rock, String("Health")}).f() == 0.0F);
    CHECK_FALSE(rig.call("caller", "dead", {rock}).b());
    CHECK(rig.vm.errors() == 2);
    CHECK(states.get(k_rock, av::k_health) == 0.0);
}

TEST_CASE("ACTV 3 keeps SetAllowFlying and the forced landing marker, version 2 still loads", "[actor][save]") {
    auto s = make();
    CHECK(s.allowed_to_fly(k_guard));
    CHECK(s.landing_marker(k_guard) == 0);
    s.set_allowed_to_fly(k_guard, false);
    s.set_landing_marker(k_guard, 0x2468);
    s.set_landing_marker(k_rock, 0x1); // no actor: ignored
    CHECK_FALSE(s.allowed_to_fly(k_guard));
    CHECK(s.allowed_to_fly(k_elder));
    const auto bytes = s.save();
    ActorStates loaded;
    REQUIRE(ActorStates::load(skydot::save::ByteReader(bytes), loaded));
    ActorStates t = make();
    t.adopt(std::move(loaded));
    CHECK_FALSE(t.allowed_to_fly(k_guard));
    CHECK(t.landing_marker(k_guard) == 0x2468);
    CHECK(t.landing_marker(k_rock) == 0);
    ActorStates scratch;
    for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
        const std::vector<std::uint8_t> shorter(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(ActorStates::load(skydot::save::ByteReader(shorter), scratch));
    }
    // Version 2: one actor with a killer and no corpse, nothing after.
    skydot::save::ByteWriter w;
    w.u32(1);
    w.u32(k_guard);
    w.u8(0);
    w.u32(0);
    w.u32(k_hero);
    w.u8(0);
    ActorStates old;
    REQUIRE(ActorStates::load(skydot::save::ByteReader(w.out), old, 2));
    ActorStates u = make();
    u.adopt(std::move(old));
    CHECK(u.killer_of(k_guard) == k_hero);
    CHECK(u.allowed_to_fly(k_guard));
    CHECK(u.landing_marker(k_guard) == 0);
}

TEST_CASE("the flight natives keep what a script says", "[actor][natives]") {
    auto states = make();
    Rig rig;
    rig.add({.name = "Actor",
             .functions = {native_function("SetAllowFlying", {{"b", "Bool"}}, "None", false),
                           native_function("SetAllowFlyingEx", {{"b", "Bool"}, {"c", "Bool"}, {"d", "Bool"}}, "None",
                                           false),
                           native_function("IsAllowedToFly", {}, "Bool", false),
                           native_function("SetForcedLandingMarker", {{"m", "ObjectReference"}}, "None", false)}});
    bind_actor_natives(rig.vm, states);
    auto allow = global_function("allow", {{"a", "Actor"}, {"b", "Bool"}});
    allow.add(Op::callmethod, {ident("SetAllowFlying"), ident("a"), ident("::NoneVar"), ident("b")})
        .add(Op::return_, {ident("::NoneVar")});
    auto allow_ex = global_function("allow_ex", {{"a", "Actor"}, {"b", "Bool"}});
    allow_ex.add(Op::callmethod, {ident("SetAllowFlyingEx"), ident("a"), ident("::NoneVar"), ident("b"), ident("b"),
                                   ident("b")})
        .add(Op::return_, {ident("::NoneVar")});
    auto allowed = global_function("allowed", {{"a", "Actor"}}, {{"r", "Bool"}}, "Bool");
    allowed.add(Op::callmethod, {ident("IsAllowedToFly"), ident("a"), ident("r")}).add(Op::return_, {ident("r")});
    auto marker = global_function("marker", {{"a", "Actor"}, {"m", "ObjectReference"}});
    marker.add(Op::callmethod, {ident("SetForcedLandingMarker"), ident("a"), ident("::NoneVar"), ident("m")})
        .add(Op::return_, {ident("::NoneVar")});
    rig.add(script("Caller", {allow, allow_ex, allowed, marker}));

    const auto guard = rig.vm.object(k_guard, "Actor");
    CHECK(rig.call("caller", "allowed", {guard}).b());
    rig.call("caller", "allow", {guard, Bool(false)});
    CHECK_FALSE(rig.call("caller", "allowed", {guard}).b());
    rig.call("caller", "allow", {guard, Bool(true)});
    CHECK(rig.call("caller", "allowed", {guard}).b());
    rig.call("caller", "allow_ex", {guard, Bool(false)});
    CHECK_FALSE(states.allowed_to_fly(k_guard));
    rig.call("caller", "marker", {guard, rig.vm.object(0x4321, "ObjectReference")});
    CHECK(states.landing_marker(k_guard) == 0x4321);
    rig.call("caller", "marker", {guard, Value{}});
    CHECK(states.landing_marker(k_guard) == 0);
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("ACTV 4 keeps ghosts and the factions scripts set, version 3 still loads", "[actor][save]") {
    auto s = make();
    CHECK_FALSE(s.is_ghost(k_guard));
    CHECK_FALSE(s.faction_override(k_guard, 0x10).has_value());
    s.set_ghost(k_guard, true);
    s.set_ghost(k_rock, true); // no actor: ignored
    s.set_faction_rank(k_guard, 0x10, 2);
    s.set_faction_rank(k_guard, 0x20, -5); // below 0 means removed
    s.set_faction_rank(k_rock, 0x10, 1);
    CHECK(s.is_ghost(k_guard));
    CHECK_FALSE(s.is_ghost(k_rock));
    CHECK(s.faction_override(k_guard, 0x10) == 2);
    CHECK(s.faction_override(k_guard, 0x20) == -1);
    CHECK(s.added_factions(k_guard) == std::vector<std::uint32_t>{0x10});
    CHECK(s.added_factions(k_rock).empty());

    const auto bytes = s.save();
    ActorStates loaded;
    REQUIRE(ActorStates::load(skydot::save::ByteReader(bytes), loaded));
    ActorStates t = make();
    t.adopt(std::move(loaded));
    CHECK(t.is_ghost(k_guard));
    CHECK(t.faction_override(k_guard, 0x10) == 2);
    CHECK(t.faction_override(k_guard, 0x20) == -1);
    CHECK_FALSE(t.faction_override(k_guard, 0x30).has_value());
    t.set_ghost(k_guard, false);
    CHECK_FALSE(t.is_ghost(k_guard));
    ActorStates scratch;
    for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
        const std::vector<std::uint8_t> shorter(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(ActorStates::load(skydot::save::ByteReader(shorter), scratch));
    }
    // Version 3: one actor, a flight setting, nothing after.
    skydot::save::ByteWriter w;
    w.u32(1);
    w.u32(k_guard);
    w.u8(0);
    w.u32(0);
    w.u32(0);
    w.u8(0);
    w.u8(1);
    w.u32(0x77);
    ActorStates old;
    REQUIRE(ActorStates::load(skydot::save::ByteReader(w.out), old, 3));
    ActorStates u = make();
    u.adopt(std::move(old));
    CHECK_FALSE(u.allowed_to_fly(k_guard));
    CHECK_FALSE(u.is_ghost(k_guard));
    CHECK_FALSE(u.faction_override(k_guard, 0x10).has_value());
}

TEST_CASE("ACTV 5 keeps the actors whose OnDeath waits, version 4 still loads", "[actor][save]") {
    auto s = make();
    CHECK_FALSE(s.is_dying(k_guard));
    CHECK_FALSE(s.finish_dying(k_guard));
    s.defer_death(k_guard, 30.0);
    s.defer_death(k_elder, 2.0);
    CHECK(s.is_dying(k_guard));
    s.tick(1.0);
    const auto bytes = s.save();
    ActorStates loaded;
    REQUIRE(ActorStates::load(skydot::save::ByteReader(bytes), loaded));
    ActorStates t = make();
    t.adopt(std::move(loaded));
    CHECK(t.is_dying(k_guard));
    CHECK(t.is_dying(k_elder));
    CHECK(t.take_due_deaths().empty());
    t.tick(1.5); // 1 s left for the elder, 29 for the guard
    CHECK(t.take_due_deaths() == std::vector<std::uint32_t>{k_elder});
    CHECK_FALSE(t.is_dying(k_elder));
    CHECK(t.finish_dying(k_guard));
    CHECK_FALSE(t.finish_dying(k_guard));
    CHECK(t.take_due_deaths().empty());
    ActorStates scratch;
    for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
        const std::vector<std::uint8_t> shorter(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(ActorStates::load(skydot::save::ByteReader(shorter), scratch));
    }
    // Version 4: no actors, nothing after.
    skydot::save::ByteWriter w;
    w.u32(0);
    ActorStates old;
    REQUIRE(ActorStates::load(skydot::save::ByteReader(w.out), old, 4));
    CHECK_FALSE(old.is_dying(k_guard));
}

TEST_CASE("Kill: the actor dies at once with its killer, essential or not; the player only loses his health",
          "[actor][death]") {
    auto s = make();
    s.kill(k_guard, k_hero);
    CHECK(s.is_dead(k_guard));
    CHECK(s.get(k_guard, av::k_health) == Approx(0));
    CHECK(s.killer_of(k_guard) == k_hero);
    CHECK(s.take_deaths() == std::vector<std::uint32_t>{k_guard});
    s.kill(k_guard, k_elder); // already dead: the first killer stays, no second death
    CHECK(s.killer_of(k_guard) == k_hero);
    CHECK(s.take_deaths().empty());
    s.kill(k_elder, 0); // essential actors are killed by a script
    CHECK(s.is_dead(k_elder));
    CHECK(s.take_deaths() == std::vector<std::uint32_t>{k_elder});
    s.kill(k_hero, k_guard);
    CHECK_FALSE(s.is_dead(k_hero));
    CHECK(s.get(k_hero, av::k_health) == Approx(0));
    CHECK(s.take_deaths().empty());
    s.kill(k_rock, k_hero); // no actor: nothing
    CHECK_FALSE(s.is_dead(k_rock));
}

TEST_CASE("SetEssential and SetInvulnerable are kept per base and win over the data", "[actor][death]") {
    auto s = make();
    CHECK_FALSE(s.is_essential(k_guard));
    CHECK(s.is_essential(k_elder));
    s.set_base_essential(k_guard + 0x1000, true);
    s.set_base_essential(k_elder + 0x1000, false);
    CHECK(s.is_essential(k_guard));
    CHECK_FALSE(s.is_essential(k_elder));
    s.damage(k_guard, av::k_health, 500); // essential: Health 0 does not kill
    CHECK_FALSE(s.is_dead(k_guard));
    CHECK(s.get(k_guard, av::k_health) == Approx(0));
    s.damage(k_elder, av::k_health, 500); // no longer essential
    CHECK(s.is_dead(k_elder));

    auto t = make();
    t.set_base_invulnerable(k_guard + 0x1000, true);
    CHECK(t.is_invulnerable(k_guard));
    t.damage(k_guard, av::k_health, 50);
    CHECK(t.get(k_guard, av::k_health) == Approx(100));
    t.damage(k_guard, av::k_stamina, 10); // only Health is spared
    CHECK(t.get(k_guard, av::k_stamina) == Approx(50));
    CHECK_FALSE(t.is_invulnerable(k_elder));
    t.set_base_invulnerable(k_guard + 0x1000, false);
    t.damage(k_guard, av::k_health, 50);
    CHECK(t.get(k_guard, av::k_health) == Approx(50));
}

TEST_CASE("relationship ranks and faction relations are kept, clamped and the same both ways round", "[actor]") {
    auto s = make();
    CHECK_FALSE(s.relationship_rank(k_guard, k_hero).has_value());
    s.set_relationship_rank(k_guard, k_hero, 3);
    CHECK(s.relationship_rank(k_hero, k_guard) == 3);
    s.set_relationship_rank(k_hero, k_guard, -9);
    CHECK(s.relationship_rank(k_guard, k_hero) == -4);

    CHECK_FALSE(s.has_faction_reactions());
    s.set_faction_reaction(0x10, 0x20, 1);
    s.set_faction_reaction(0x20, 0x10, 0);
    s.set_faction_reaction(0x10, 0x30, 3);
    s.set_faction_reaction(0x11, 0x20, 2);
    CHECK(s.has_faction_reactions());
    CHECK(s.faction_reaction(0x10, 0x20) == 1);
    CHECK(s.faction_reaction(0x20, 0x10) == 0);
    CHECK_FALSE(s.faction_reaction(0x30, 0x10).has_value());
    const std::vector<std::pair<std::uint32_t, std::uint8_t>> from10 = {{0x20u, std::uint8_t{1}}, {0x30u, std::uint8_t{3}}};
    CHECK(s.faction_reactions_from(0x10) == from10);
    CHECK(s.faction_reactions_from(0x12).empty());
}

TEST_CASE("ACTV 6 keeps base flags, relationships and faction relations, version 5 still loads", "[actor][save]") {
    auto s = make();
    s.set_base_essential(0x1100, true);
    s.set_base_invulnerable(0x1100, false);
    s.set_base_invulnerable(0x1101, true);
    s.set_relationship_rank(k_guard, k_hero, -2);
    s.set_faction_reaction(0x10, 0x20, 1);
    s.set_faction_reaction(0x20, 0x10, 3);
    const auto bytes = s.save();
    ActorStates loaded;
    REQUIRE(ActorStates::load(skydot::save::ByteReader(bytes), loaded));
    ActorStates t = make();
    t.adopt(std::move(loaded));
    CHECK(t.is_essential(k_guard));        // 0x1100 is the guard's base
    CHECK_FALSE(t.is_invulnerable(k_guard));
    CHECK(t.is_invulnerable(k_elder));     // 0x1101
    CHECK(t.relationship_rank(k_hero, k_guard) == -2);
    CHECK(t.faction_reaction(0x10, 0x20) == 1);
    CHECK(t.faction_reaction(0x20, 0x10) == 3);
    ActorStates scratch;
    for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
        const std::vector<std::uint8_t> shorter(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(ActorStates::load(skydot::save::ByteReader(shorter), scratch));
    }
    // Version 5 ends after the dying actors: the same bytes without the three counts load, and bytes left over
    // from a newer version do not.
    auto empty = make().save();
    REQUIRE(empty.size() > 16);
    const std::vector<std::uint8_t> v5(empty.begin(), empty.end() - 16);
    ActorStates old;
    REQUIRE(ActorStates::load(skydot::save::ByteReader(v5), old, 5));
    CHECK_FALSE(old.has_faction_reactions());
    CHECK_FALSE(ActorStates::load(skydot::save::ByteReader(empty), scratch, 5));
    CHECK(ActorStates::load(skydot::save::ByteReader(empty), scratch, 7));
}

TEST_CASE("ACTV 7 keeps where living actors stood, version 6 still loads", "[actor][save]") {
    auto s = make();
    s.set_living_place(k_guard, {.space = 0x3C, .x = 1.5, .y = -2.5, .z = 300.0, .rot_z = 0.25});
    s.set_living_place(k_rock, {.space = 0x3C}); // no actor: ignored
    CHECK(s.living_place(k_guard)->space == 0x3C);
    CHECK_FALSE(s.living_place(k_rock).has_value());
    const auto bytes = s.save();
    ActorStates loaded;
    REQUIRE(ActorStates::load(skydot::save::ByteReader(bytes), loaded));
    ActorStates t = make();
    t.adopt(std::move(loaded));
    const auto place = t.living_place(k_guard);
    REQUIRE(place.has_value());
    CHECK(place->x == Approx(1.5));
    CHECK(place->y == Approx(-2.5));
    CHECK(place->z == Approx(300.0));
    CHECK(place->rot_z == Approx(0.25));
    ActorStates scratch;
    for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
        const std::vector<std::uint8_t> shorter(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK_FALSE(ActorStates::load(skydot::save::ByteReader(shorter), scratch));
    }
    // Version 6 ends after the faction relations: the same bytes without the place count load.
    auto empty = make().save();
    const std::vector<std::uint8_t> v6(empty.begin(), empty.end() - 4);
    ActorStates old;
    REQUIRE(ActorStates::load(skydot::save::ByteReader(v6), old, 6));
    CHECK_FALSE(old.living_place(k_guard).has_value());
    t.clear_living_places();
    CHECK_FALSE(t.living_place(k_guard).has_value());
}
