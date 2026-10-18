// SPDX-License-Identifier: GPL-3.0-or-later
//
// Magic (docs/adr/0008): the pure rules (cost, resistance, area) and the active effects over a
// fake host backed by ActorStates: value modifiers once and per second, held modifiers, peak,
// dual, absorb, conditions, refresh, death, unhandled archetypes and the saved section.
#include "support/world_builder.hpp"

#include "data/fb_search.hpp"
#include "magic/active_effects.hpp"
#include "magic/magic_rules.hpp"
#include "vm/actor_states.hpp"

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
constexpr std::uint32_t k_stamina = vm::av::k_stamina;

// MGEF ids.
constexpr std::uint32_t k_restore = 0x1001;      // restore health, instant
constexpr std::uint32_t k_restore_conc = 0x1002; // restore health per second
constexpr std::uint32_t k_fire = 0x1003;         // fire damage per second, resisted by ResistFire
constexpr std::uint32_t k_fortify = 0x1004;      // fortify health: peak, recover
constexpr std::uint32_t k_frost = 0x1005;        // dual: health and stamina
constexpr std::uint32_t k_drain = 0x1006;        // absorb health
constexpr std::uint32_t k_cond = 0x1007;         // restore with a condition on its MGEF
constexpr std::uint32_t k_demoralize = 0x1008;   // archetype 7: not done
constexpr std::uint32_t k_nodeath = 0x1009;      // fortify that survives death
constexpr std::uint32_t k_weak = 0x100A;         // weakness to fire: value modifier, detrimental, recover
constexpr std::uint32_t k_script = 0x100B;       // archetype 1

constexpr std::uint32_t k_spell = 0x2001;
constexpr std::uint32_t k_spell2 = 0x2002;
constexpr std::uint32_t k_spell_cond = 0x2003;

bool fake_source(std::uint32_t ref, vm::ActorBase& out) {
    if (ref != k_player && ref != k_skeever) {
        return false;
    }
    out = vm::ActorBase{};
    out.base[k_health] = 100;
    out.base[k_magicka] = 80;
    out.base[k_stamina] = 60;
    out.base[vm::av::k_first_skill + 14] = 15; // Destruction
    out.is_player = ref == k_player;
    return true;
}

struct Host : magic::MagicHost {
    vm::ActorStates states;
    bool conditions_hold = true;
    int condition_calls = 0;
    std::vector<std::string> logs;
    std::vector<std::pair<bool, std::uint32_t>> events; // started, mgef

    Host() { states.set_source(fake_source); }
    double actor_value(std::uint32_t ref, std::uint32_t av) const override { return states.get(ref, av); }
    void damage_value(std::uint32_t ref, std::uint32_t av, double amount) override { states.damage(ref, av, amount); }
    void restore_value(std::uint32_t ref, std::uint32_t av, double amount) override { states.restore(ref, av, amount); }
    void modify_temp(std::uint32_t ref, std::uint32_t av, double delta) override { states.mod_temp(ref, av, delta); }
    bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<magic::wfb::Condition>>*, std::uint32_t,
                         std::uint32_t) override {
        ++condition_calls;
        return conditions_hold;
    }
    bool is_dead(std::uint32_t ref) const override { return states.is_dead(ref); }
    void effect_changed(bool started, const magic::EffectView& e) override { events.emplace_back(started, e.mgef); }
    void log(const std::string& line) override { logs.push_back(line); }
    [[nodiscard]] int count_logs(const std::string& needle) const {
        return static_cast<int>(std::ranges::count_if(
            logs, [&](const std::string& l) { return l.find(needle) != std::string::npos; }));
    }
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
    auto restore = mgef(k_restore, 0, static_cast<std::int32_t>(k_health), 0, 1.0F);
    auto restore_conc = mgef(k_restore_conc, 0, static_cast<std::int32_t>(k_health), 0, 1.0F);
    restore_conc.casting_type = 2;
    auto fire = mgef(k_fire, 0, static_cast<std::int32_t>(k_health), magic::mgef_flag::hostile | magic::mgef_flag::detrimental, 2.5F);
    fire.resist_value = 41;
    fire.casting_type = 2;
    auto fortify = mgef(k_fortify, 34, static_cast<std::int32_t>(k_health), magic::mgef_flag::recover, 0.5F);
    fortify.keywords = {0x77};
    auto frost = mgef(k_frost, 5, static_cast<std::int32_t>(k_health), magic::mgef_flag::hostile | magic::mgef_flag::detrimental, 1.0F);
    frost.second_actor_value = static_cast<std::int32_t>(k_stamina);
    frost.second_av_weight = 0.5F;
    auto drain = mgef(k_drain, 4, static_cast<std::int32_t>(k_health), magic::mgef_flag::hostile | magic::mgef_flag::detrimental, 1.0F);
    auto cond = mgef(k_cond, 0, static_cast<std::int32_t>(k_health), 0, 1.0F);
    cond.conditions = {condition(448, ge, 1.0F)};
    auto demoralize = mgef(k_demoralize, 7, -1, magic::mgef_flag::hostile, 1.0F);
    auto nodeath = mgef(k_nodeath, 34, static_cast<std::int32_t>(k_magicka), magic::mgef_flag::recover | magic::mgef_flag::no_death_dispel, 0.5F);
    auto weak = mgef(k_weak, 0, 41, magic::mgef_flag::recover | magic::mgef_flag::detrimental, 0.5F);
    auto script = mgef(k_script, 1, -1, 0, 0.0F);
    spec.magic_effects = {restore, restore_conc, fire, fortify, frost, drain, cond, demoralize, nodeath, weak, script};
    spec.spells = {
        SpellSpec{.id = k_spell, .name = "Flames", .cost = 14, .casting_type = 2, .delivery = 2,
                  .effects = {{.effect = k_fire, .magnitude = 8.0F, .duration = 1}}},
        SpellSpec{.id = k_spell2, .name = "Fortify", .cost = 20,
                  .effects = {{.effect = k_fortify, .magnitude = 30.0F, .duration = 10}}},
        SpellSpec{.id = k_spell_cond, .name = "Cond", .cost = 0,
                  .effects = {{.effect = k_restore_conc, .magnitude = 10.0F, .duration = 1,
                               .conditions = {condition(448, ge, 1.0F)}}}},
    };
    return spec;
}

struct Rig {
    BuiltWorld world{world_spec()};
    Host host;
    magic::ActiveEffects effects{*world, host};

    std::vector<std::uint32_t> cast(std::uint32_t caster, std::uint32_t target, std::uint32_t spell) {
        const auto* s = skydot::lookup(world->spells(), spell);
        REQUIRE(s != nullptr);
        const auto inputs = magic::inputs_of(s->effects());
        return effects.apply(caster, target, {magic::SourceKind::spell, spell, 0}, inputs);
    }
    std::vector<std::uint32_t> raw(std::uint32_t caster, std::uint32_t target, magic::Source source,
                                   std::uint32_t mgef, float magnitude, std::uint32_t duration,
                                   magic::ApplyOptions options = {}, std::int32_t index = -1) {
        const magic::EffectInput in{mgef, magnitude, 0, duration, index};
        return effects.apply(caster, target, source, std::span(&in, 1), options);
    }
    void run(double seconds, double step = 0.1) {
        for (double t = 0; t < seconds - 1e-9; t += step) {
            effects.tick(step);
            host.states.tick(step);
        }
    }
};

} // namespace

// ---- the rules ------------------------------------------------------------------------------

TEST_CASE("the skill multiplier is 1 - (skill/400)^0.65", "[magic][rules]") {
    CHECK(magic::skill_cost_multiplier(0) == Approx(1.0));
    CHECK(magic::skill_cost_multiplier(100) == Approx(0.5939).margin(0.0005)); // UESP: about 0.5939
    CHECK(magic::skill_cost_multiplier(15) == Approx(0.8817).margin(0.0005));
    CHECK(magic::skill_cost_multiplier(400) == Approx(0.0));
}

TEST_CASE("effect costs follow the stored costs of Skyrim.esm", "[magic][rules]") {
    // Fitted to the spells of Skyrim.esm without the manual flag (adr/0008).
    CHECK(std::floor(magic::effect_cost(5.0, 30, 2, magic::casting::fire_and_forget, 0)) == 35); // TrapFirePlateHazardSpell06
    CHECK(std::floor(magic::effect_cost(5.0, 13, 2, magic::casting::fire_and_forget, 0)) == 14); // ...01
    CHECK(std::floor(magic::effect_cost(15.0, 0, 60, magic::casting::fire_and_forget, 0)) == 107); // ConjureFamiliarRightHand
    CHECK(std::floor(magic::effect_cost(1.5, 8, 1, magic::casting::concentration, 0)) == 14);   // PowerDarkElfFlameCloakDmg
    CHECK(std::floor(magic::effect_cost(1.0, 30, 0, magic::casting::fire_and_forget, 15)) == 42); // TrapFireball01: no area term
    CHECK(std::floor(magic::effect_cost(1.5, 0, 300, magic::casting::fire_and_forget, 0)) == 63); // BoundSwordLeftHand
}

TEST_CASE("resistance is capped and area is in feet", "[magic][rules]") {
    CHECK(magic::resisted_magnitude(100, 50) == Approx(50));
    CHECK(magic::resisted_magnitude(100, 120) == Approx(15)); // capped at 85
    CHECK(magic::resisted_magnitude(100, -20) == Approx(100));
    CHECK(magic::area_units(10) == Approx(213.33).margin(0.01));
    CHECK(magic::archetype_name(34) == "PeakValueModifier");
    CHECK(magic::archetype_name(99) == "Unknown99");
}

TEST_CASE("a spell costs its stored cost times the skill multiplier", "[magic][rules]") {
    Rig rig;
    const auto* flames = skydot::lookup(rig.world->spells(), k_spell);
    REQUIRE(flames != nullptr);
    CHECK(magic::spell_cost(*rig.world, *flames, 15) == Approx(14 * 0.8817).margin(0.01));
    CHECK(magic::spell_cost(*rig.world, *flames, 100) == Approx(14 * 0.5939).margin(0.01));
    CHECK(magic::auto_calc_cost(*rig.world, *flames) == Approx(2.5 * std::pow(8.0, 1.1)).margin(0.01));
}

// ---- value modifiers -------------------------------------------------------------------------

TEST_CASE("duration 0 acts once and restore never goes above the maximum", "[magic][effects]") {
    Rig rig;
    rig.host.states.damage(k_player, k_health, 30);
    CHECK(rig.raw(k_player, k_player, {magic::SourceKind::potion, 0x3EADE, 0}, k_restore, 50, 0).size() == 1);
    CHECK(rig.host.states.get(k_player, k_health) == Approx(100)); // 70 + 50 stops at the maximum
    CHECK(rig.effects.size() == 0);                                 // it is over at once
    CHECK(rig.host.events.size() == 2);                             // started and finished
    CHECK(rig.host.count_logs("effect 0x1001 on 0x14 start mag 50") == 1);
    CHECK(rig.host.count_logs("effect 0x1001 on 0x14 end") == 1);
}

TEST_CASE("a timed effect without Recover acts magnitude per second and then ends", "[magic][effects]") {
    Rig rig;
    rig.host.states.damage(k_player, k_health, 60);
    rig.raw(k_player, k_player, {magic::SourceKind::potion, 1, 0}, k_restore_conc, 10, 3);
    rig.run(1.0);
    CHECK(rig.host.states.get(k_player, k_health) == Approx(50).margin(0.01)); // 40 + 10
    rig.run(5.0);
    CHECK(rig.host.states.get(k_player, k_health) == Approx(70).margin(0.01)); // three seconds, not eight
    CHECK(rig.effects.size() == 0);
}

TEST_CASE("a hostile effect is cut by the resistance of its kind and by MagicResist", "[magic][effects]") {
    Rig rig;
    rig.host.states.set_base(k_skeever, 41, 50); // ResistFire 50 %
    rig.cast(k_player, k_skeever, k_spell);
    REQUIRE(rig.effects.effects_on(k_skeever).size() == 1);
    CHECK(rig.effects.effects_on(k_skeever)[0].magnitude == Approx(4.0));
    rig.run(1.0);
    CHECK(rig.host.states.get(k_skeever, k_health) == Approx(96).margin(0.01));
    rig.effects.dispel_all(k_skeever);
    rig.host.states.set_base(k_skeever, 44, 50); // and MagicResist 50 % on top
    rig.cast(k_player, k_skeever, k_spell);
    CHECK(rig.effects.effects_on(k_skeever)[0].magnitude == Approx(2.0));
}

TEST_CASE("a recast refreshes the instance, another source stacks", "[magic][effects]") {
    Rig rig;
    const auto a = rig.cast(k_player, k_skeever, k_spell);
    rig.run(0.5);
    const auto b = rig.cast(k_player, k_skeever, k_spell); // concentration: the caller re-applies
    CHECK(a == b);
    CHECK(rig.effects.effects_on(k_skeever)[0].elapsed == Approx(0.0));
    CHECK(rig.effects.size() == 1);
    rig.raw(k_player, k_skeever, {magic::SourceKind::spell, 0x9999, 0}, k_fire, 8, 1);
    CHECK(rig.effects.size() == 2);
}

// ---- held modifiers ----------------------------------------------------------------------------

TEST_CASE("Recover holds a temporary modifier and takes it away at the end", "[magic][effects]") {
    Rig rig;
    rig.host.states.damage(k_player, k_health, 10);
    rig.cast(k_player, k_player, k_spell2);
    CHECK(rig.host.states.max(k_player, k_health) == Approx(130));
    CHECK(rig.host.states.get(k_player, k_health) == Approx(120)); // fortify raises the current value too
    CHECK(rig.effects.has_effect(k_player, k_fortify));
    CHECK(rig.effects.has_effect_keyword(k_player, 0x77));
    CHECK_FALSE(rig.effects.has_effect_keyword(k_player, 0x78));
    rig.run(9.5);
    CHECK(rig.host.states.max(k_player, k_health) == Approx(130));
    rig.run(1.0);
    CHECK(rig.host.states.max(k_player, k_health) == Approx(100));
    CHECK(rig.host.states.get(k_player, k_health) == Approx(90));
    CHECK(rig.host.states.get_temp(k_player, k_health) == Approx(0));
    CHECK_FALSE(rig.effects.has_effect(k_player, k_fortify));
}

TEST_CASE("peak value modifiers: only the largest counts, and the next takes over", "[magic][effects]") {
    Rig rig;
    rig.raw(k_player, k_player, {magic::SourceKind::potion, 1, 0}, k_fortify, 20, 100);
    CHECK(rig.host.states.max(k_player, k_health) == Approx(120));
    rig.raw(k_player, k_player, {magic::SourceKind::potion, 2, 0}, k_fortify, 50, 10);
    CHECK(rig.host.states.max(k_player, k_health) == Approx(150)); // not 170
    rig.raw(k_player, k_player, {magic::SourceKind::potion, 3, 0}, k_fortify, 35, 100);
    CHECK(rig.host.states.max(k_player, k_health) == Approx(150));
    rig.run(10.5);
    CHECK(rig.host.states.max(k_player, k_health) == Approx(135)); // the 35 takes over from the 50
    rig.effects.dispel(k_player, 3);
    CHECK(rig.host.states.max(k_player, k_health) == Approx(120));
    rig.effects.dispel_all(k_player);
    CHECK(rig.host.states.max(k_player, k_health) == Approx(100));
}

TEST_CASE("a detrimental held modifier subtracts, also on a resistance", "[magic][effects]") {
    Rig rig;
    rig.host.states.set_base(k_skeever, 41, 20);
    rig.raw(k_player, k_skeever, {magic::SourceKind::spell, 5, 0}, k_weak, 25, 30, {.constant = false});
    CHECK(rig.host.states.max(k_skeever, 41) == Approx(20 - 25 * (1 - 0.0))); // hostile flag is not set: no resist
    rig.effects.dispel_all(k_skeever);
    CHECK(rig.host.states.max(k_skeever, 41) == Approx(20));
}

TEST_CASE("a constant effect holds until it is dispelled", "[magic][effects]") {
    Rig rig;
    rig.raw(k_player, k_player, {magic::SourceKind::ability, 0x3000, 0}, k_fortify, 25, 0);
    CHECK(rig.host.states.max(k_player, k_health) == Approx(125));
    rig.run(120.0, 1.0);
    CHECK(rig.host.states.max(k_player, k_health) == Approx(125));
    CHECK(rig.effects.dispel(k_player, 0x3000) == 1);
    CHECK(rig.host.states.max(k_player, k_health) == Approx(100));
}

// ---- dual and absorb ---------------------------------------------------------------------------

TEST_CASE("a dual value modifier hits the second actor value with magnitude x weight", "[magic][effects]") {
    Rig rig;
    rig.raw(k_player, k_skeever, {magic::SourceKind::spell, 7, 0}, k_frost, 10, 2);
    rig.run(2.0);
    CHECK(rig.host.states.get(k_skeever, k_health) == Approx(80).margin(0.01));
    CHECK(rig.host.states.get(k_skeever, k_stamina) == Approx(50).margin(0.01));
}

TEST_CASE("absorb takes from the target and gives to the caster", "[magic][effects]") {
    Rig rig;
    rig.host.states.damage(k_player, k_health, 40);
    rig.raw(k_player, k_skeever, {magic::SourceKind::spell, 8, 0}, k_drain, 10, 2);
    rig.run(2.0);
    CHECK(rig.host.states.get(k_skeever, k_health) == Approx(80).margin(0.01));
    CHECK(rig.host.states.get(k_player, k_health) == Approx(80).margin(0.01)); // 60 + 20
}

// ---- conditions, death, archetypes --------------------------------------------------------------

TEST_CASE("an effect whose conditions fail stays inactive and does not act", "[magic][effects]") {
    Rig rig;
    rig.host.states.damage(k_player, k_health, 50);
    rig.host.conditions_hold = false;
    rig.cast(k_player, k_player, k_spell_cond);
    REQUIRE(rig.effects.effects_on(k_player).size() == 1);
    CHECK_FALSE(rig.effects.effects_on(k_player)[0].active);
    CHECK_FALSE(rig.effects.has_effect(k_player, k_restore_conc));
    rig.run(0.9);
    CHECK(rig.host.states.get(k_player, k_health) == Approx(50));
    CHECK(rig.host.events.empty()); // it never started
    rig.host.conditions_hold = true;
    rig.cast(k_player, k_player, k_spell_cond); // refreshed: the duration restarts
    REQUIRE(rig.effects.effects_on(k_player).size() == 1);
    CHECK(rig.effects.effects_on(k_player)[0].active);
    rig.run(0.5);
    CHECK(rig.host.states.get(k_player, k_health) == Approx(55).margin(0.01));
}

TEST_CASE("the MGEF's own conditions apply too", "[magic][effects]") {
    Rig rig;
    rig.host.conditions_hold = false;
    CHECK(rig.raw(k_player, k_player, {magic::SourceKind::potion, 4, 0}, k_cond, 10, 0).empty());
    CHECK(rig.host.condition_calls > 0);
}

TEST_CASE("death removes effects except the no-death-dispel ones", "[magic][effects]") {
    Rig rig;
    rig.raw(k_player, k_skeever, {magic::SourceKind::potion, 1, 0}, k_fortify, 20, 100);
    rig.raw(k_player, k_skeever, {magic::SourceKind::potion, 2, 0}, k_nodeath, 20, 100);
    CHECK(rig.effects.effects_on(k_skeever).size() == 2);
    rig.effects.on_death(k_skeever);
    REQUIRE(rig.effects.effects_on(k_skeever).size() == 1);
    CHECK(rig.effects.effects_on(k_skeever)[0].mgef == k_nodeath);
    CHECK(rig.host.states.max(k_skeever, k_health) == Approx(100));
}

TEST_CASE("archetypes without rules are logged once, counted, and live their duration", "[magic][effects]") {
    Rig rig;
    rig.raw(k_player, k_skeever, {magic::SourceKind::spell, 1, 0}, k_demoralize, 5, 15);
    rig.raw(k_player, k_skeever, {magic::SourceKind::spell, 2, 0}, k_demoralize, 5, 15);
    CHECK(rig.host.count_logs("magic: archetype 7 (Demoralize) not done") == 1);
    CHECK(rig.effects.unhandled().at(7) == 2);
    CHECK(rig.effects.has_effect(k_skeever, k_demoralize));
    rig.run(14.0);
    CHECK(rig.effects.has_effect(k_skeever, k_demoralize));
    rig.run(1.5);
    CHECK_FALSE(rig.effects.has_effect(k_skeever, k_demoralize));
}

TEST_CASE("a script effect starts and finishes with an event each", "[magic][effects]") {
    Rig rig;
    rig.raw(k_player, k_player, {magic::SourceKind::spell, 1, 0}, k_script, 0, 2);
    REQUIRE(rig.host.events.size() == 1);
    CHECK(rig.host.events[0] == std::make_pair(true, k_script));
    rig.run(2.5);
    REQUIRE(rig.host.events.size() == 2);
    CHECK(rig.host.events[1] == std::make_pair(false, k_script));
    CHECK(rig.host.count_logs("not done") == 0);
}

TEST_CASE("dual casting is a placeholder, logged once", "[magic][effects]") {
    Rig rig;
    rig.raw(k_player, k_player, {magic::SourceKind::spell, 1, 0}, k_restore, 5, 0, {.dual_cast = true});
    rig.raw(k_player, k_player, {magic::SourceKind::spell, 1, 0}, k_restore, 5, 0, {.dual_cast = true});
    CHECK(rig.host.count_logs("dual casting is a placeholder") == 1);
}

// ---- saving ---------------------------------------------------------------------------------------

TEST_CASE("the temporary modifier is not saved and the effects hold it again after a load", "[magic][effects][save]") {
    Rig rig;
    rig.host.states.damage(k_player, k_health, 20);
    rig.raw(k_player, k_player, {magic::SourceKind::potion, 1, 0}, k_fortify, 40, 30);
    rig.run(10.0);
    CHECK(rig.host.states.max(k_player, k_health) == Approx(140));
    const auto actors = rig.host.states.save();
    const auto saved = rig.effects.save();

    // A new session: the actor values come back without the temporary part.
    Rig other;
    REQUIRE(vm::ActorStates::load(skydot::save::ByteReader(actors), other.host.states));
    other.host.states.set_source(fake_source);
    CHECK(other.host.states.max(k_player, k_health) == Approx(100));
    REQUIRE(other.effects.load(skydot::save::ByteReader(saved)));
    CHECK(other.host.events.empty()); // a load starts nothing: the VM restores its own objects
    CHECK(other.host.states.max(k_player, k_health) == Approx(140)); // once, not twice
    CHECK(other.host.states.get(k_player, k_health) == Approx(120));
    REQUIRE(other.effects.effects_on(k_player).size() == 1);
    CHECK(other.effects.effects_on(k_player)[0].elapsed == Approx(10.0).margin(0.2));
    other.run(19.0);
    CHECK(other.host.states.max(k_player, k_health) == Approx(140));
    other.run(1.5);
    CHECK(other.host.states.max(k_player, k_health) == Approx(100));
    CHECK(other.host.states.get_temp(k_player, k_health) == Approx(0));
    CHECK(other.host.events.size() == 1); // only the end
}

TEST_CASE("a damaged section is refused", "[magic][effects][save]") {
    Rig rig;
    rig.raw(k_player, k_player, {magic::SourceKind::potion, 1, 0}, k_fortify, 40, 30);
    auto bytes = rig.effects.save();
    Rig other;
    CHECK(other.effects.load(skydot::save::ByteReader(bytes)));
    bytes.pop_back();
    CHECK_FALSE(other.effects.load(skydot::save::ByteReader(bytes)));
    bytes.push_back(0);
    bytes.push_back(0);
    CHECK_FALSE(other.effects.load(skydot::save::ByteReader(bytes)));
    CHECK_FALSE(other.effects.load(skydot::save::ByteReader(std::vector<std::uint8_t>{})));
}

TEST_CASE("actor states: the temporary bucket counts in value and maximum and is not saved", "[magic][actors]") {
    vm::ActorStates s;
    s.set_source(fake_source);
    s.mod_temp(k_player, k_health, 30);
    CHECK(s.max(k_player, k_health) == Approx(130));
    CHECK(s.get(k_player, k_health) == Approx(130));
    s.damage(k_player, k_health, 20);
    s.mod_temp(k_player, k_health, -30); // the maximum drops: the value is clamped to it
    CHECK(s.max(k_player, k_health) == Approx(100));
    CHECK(s.get(k_player, k_health) == Approx(80));
    s.mod_temp(k_player, k_health, 30);
    vm::ActorStates loaded;
    REQUIRE(vm::ActorStates::load(skydot::save::ByteReader(s.save()), loaded));
    loaded.set_source(fake_source);
    CHECK(loaded.get_temp(k_player, k_health) == Approx(0));
    CHECK(loaded.max(k_player, k_health) == Approx(100));
}
