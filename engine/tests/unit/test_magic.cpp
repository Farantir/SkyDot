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
#include <map>
#include <tuple>
#include <set>
#include <string>

using namespace skydot::testing;
namespace magic = skydot::magic;
namespace vm = skydot::vm;
using Catch::Approx;

namespace {

constexpr std::uint32_t k_player = 0x14;
constexpr std::uint32_t k_skeever = 0x200;
constexpr std::uint32_t k_wolf = 0x201;
constexpr std::uint32_t k_corpse = 0x202;
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
constexpr std::uint32_t k_demoralize = 0x1008;   // archetype 7: fear
constexpr std::uint32_t k_nodeath = 0x1009;      // fortify that survives death
constexpr std::uint32_t k_weak = 0x100A;         // weakness to fire: value modifier, detrimental, recover
constexpr std::uint32_t k_script = 0x100B;       // archetype 1
constexpr std::uint32_t k_disarm = 0x100C;       // archetype 9: not done
constexpr std::uint32_t k_calm = 0x1010;
constexpr std::uint32_t k_frenzy = 0x1011;
constexpr std::uint32_t k_paralysis = 0x1012;
constexpr std::uint32_t k_invis = 0x1013;
constexpr std::uint32_t k_summon = 0x1014;
constexpr std::uint32_t k_reanimate = 0x1015;
constexpr std::uint32_t k_bound = 0x1016;
constexpr std::uint32_t k_cloak = 0x1017;
constexpr std::uint32_t k_slow = 0x1018;
constexpr std::uint32_t k_stagger = 0x1019;
constexpr std::uint32_t k_turn = 0x101A;
constexpr std::uint32_t k_npc_base = 0x5001;   // the summoned creature
constexpr std::uint32_t k_weapon = 0x6001;     // the bound weapon
constexpr std::uint32_t k_cloak_spell = 0x2004; // fire damage to those near

constexpr std::uint32_t k_spell = 0x2001;
constexpr std::uint32_t k_spell2 = 0x2002;
constexpr std::uint32_t k_spell_cond = 0x2003;

bool fake_source(std::uint32_t ref, vm::ActorBase& out) {
    if (ref != k_player && ref != k_skeever && ref != k_wolf && ref != k_corpse) {
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
    std::map<std::uint32_t, std::uint32_t> levels;
    std::set<std::uint32_t> dead_refs; // summons that died
    std::set<std::uint32_t> damaged;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> placed; // caster, base
    std::vector<std::uint32_t> removed;
    std::vector<std::uint32_t> ended_reanimate;
    std::vector<std::tuple<bool, std::uint32_t, std::uint32_t>> bound; // added, target, weapon
    std::vector<std::uint32_t> near;
    mutable double last_radius = 0.0;
    bool fail_summon = false;
    std::uint32_t next_summon = 0x900;
    std::vector<std::string> logs;
    std::vector<std::pair<bool, std::uint32_t>> events; // started, mgef

    Host() { states.set_source(fake_source); }
    double actor_value(std::uint32_t ref, std::uint32_t av) const override { return states.get(ref, av); }
    void damage_value(std::uint32_t ref, std::uint32_t av, double amount, std::uint32_t) override {
        damaged.insert(ref);
        states.damage(ref, av, amount);
    }
    void restore_value(std::uint32_t ref, std::uint32_t av, double amount) override { states.restore(ref, av, amount); }
    void modify_temp(std::uint32_t ref, std::uint32_t av, double delta) override { states.mod_temp(ref, av, delta); }
    bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<magic::wfb::Condition>>*, std::uint32_t,
                         std::uint32_t) override {
        ++condition_calls;
        return conditions_hold;
    }
    bool is_dead(std::uint32_t ref) const override { return states.is_dead(ref) || dead_refs.contains(ref); }
    std::uint32_t actor_level(std::uint32_t ref) const override {
        const auto it = levels.find(ref);
        return it == levels.end() ? 0 : it->second;
    }
    std::uint32_t place_summon(std::uint32_t caster, std::uint32_t base) override {
        if (fail_summon) {
            return 0;
        }
        placed.emplace_back(caster, base);
        return next_summon++;
    }
    void remove_summon(std::uint32_t ref) override { removed.push_back(ref); }
    void reanimate(std::uint32_t target, std::uint32_t) override { states.revive(target); }
    void end_reanimate(std::uint32_t target) override {
        ended_reanimate.push_back(target);
        states.damage(target, k_health, 1e9);
    }
    void bound_add(std::uint32_t target, std::uint32_t weapon) override { bound.emplace_back(true, target, weapon); }
    void bound_remove(std::uint32_t target, std::uint32_t weapon) override { bound.emplace_back(false, target, weapon); }
    std::vector<std::uint32_t> actors_near(std::uint32_t, double radius) const override {
        last_radius = radius;
        return near;
    }
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
    auto disarm = mgef(k_disarm, 9, -1, magic::mgef_flag::hostile, 1.0F);
    disarm.editor_id = "Disarm";
    demoralize.editor_id = "Fear";
    const auto arch = [&](std::uint32_t id, std::uint32_t a, std::uint32_t assoc = 0) {
        auto m = mgef(id, a, -1, 0, 1.0F);
        m.associated_item = assoc;
        return m;
    };
    const auto calm = arch(k_calm, 6);
    const auto frenzy = arch(k_frenzy, 8);
    const auto paralysis = arch(k_paralysis, 21);
    const auto invis = arch(k_invis, 11);
    const auto summon = arch(k_summon, 18, k_npc_base);
    const auto reanimate = arch(k_reanimate, 22);
    const auto bound = arch(k_bound, 17, k_weapon);
    const auto cloak = arch(k_cloak, 35, k_cloak_spell);
    const auto slow = arch(k_slow, 37);
    const auto stagger = arch(k_stagger, 33);
    const auto turn = arch(k_turn, 24);
    auto nodeath = mgef(k_nodeath, 34, static_cast<std::int32_t>(k_magicka), magic::mgef_flag::recover | magic::mgef_flag::no_death_dispel, 0.5F);
    auto weak = mgef(k_weak, 0, 41, magic::mgef_flag::recover | magic::mgef_flag::detrimental, 0.5F);
    auto script = mgef(k_script, 1, -1, 0, 0.0F);
    spec.magic_effects = {restore, restore_conc, fire, fortify, frost, drain, cond, demoralize, nodeath, weak, script, disarm, calm, frenzy, paralysis, invis, summon, reanimate, bound, cloak, slow, stagger, turn};
    spec.spells = {
        SpellSpec{.id = k_spell, .name = "Flames", .cost = 14, .casting_type = 2, .delivery = 2,
                  .effects = {{.effect = k_fire, .magnitude = 8.0F, .duration = 1}}},
        SpellSpec{.id = k_spell2, .name = "Fortify", .cost = 20,
                  .effects = {{.effect = k_fortify, .magnitude = 30.0F, .duration = 10}}},
        SpellSpec{.id = k_spell_cond, .name = "Cond", .cost = 0,
                  .effects = {{.effect = k_restore_conc, .magnitude = 10.0F, .duration = 1,
                               .conditions = {condition(448, ge, 1.0F)}}}},
        SpellSpec{.id = k_cloak_spell, .name = "Cloak damage", .cost = 0,
                  .effects = {{.effect = k_fire, .magnitude = 8.0F, .duration = 1}}},
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
    rig.raw(k_player, k_skeever, {magic::SourceKind::spell, 1, 0}, k_disarm, 5, 15);
    rig.raw(k_player, k_skeever, {magic::SourceKind::spell, 2, 0}, k_disarm, 5, 15);
    CHECK(rig.host.count_logs("magic: archetype 9 (Disarm) not done") == 1);
    CHECK(rig.effects.unhandled().at(9) == 2);
    CHECK(rig.effects.has_effect(k_skeever, k_disarm));
    rig.run(14.0);
    CHECK(rig.effects.has_effect(k_skeever, k_disarm));
    rig.run(1.5);
    CHECK_FALSE(rig.effects.has_effect(k_skeever, k_disarm));
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

// ---- archetypes (GAMEPLAY-PLAN 7a-4) ----------------------------------------------------------

namespace {
constexpr magic::Source k_src{magic::SourceKind::spell, 0x3001, 0};
constexpr magic::Source k_src2{magic::SourceKind::spell, 0x3002, 0};
} // namespace

TEST_CASE("calm, fear and frenzy only affect targets up to the magnitude's level", "[magic][archetype]") {
    Rig rig;
    rig.host.levels[k_skeever] = 20;
    CHECK(rig.raw(k_player, k_skeever, k_src, k_calm, 15, 30).empty()); // level 20 > 15
    CHECK(rig.raw(k_player, k_skeever, k_src, k_calm, 15, 30).empty());
    CHECK(rig.host.count_logs("is too strong for") == 1);                // once per target and effect
    CHECK_FALSE(rig.effects.overrides(k_skeever).calm);
    CHECK(rig.raw(k_player, k_skeever, k_src, k_frenzy, 25, 30).size() == 1); // level 20 <= 25
    CHECK(rig.effects.overrides(k_skeever).frenzy);
    CHECK(rig.raw(k_player, k_skeever, k_src, k_demoralize, 19, 30).empty());
    CHECK(rig.raw(k_player, k_skeever, k_src, k_turn, 19, 30).empty());
    CHECK(rig.host.count_logs("(level 20 > 19)") == 2);
    CHECK(rig.effects.unhandled().empty());
}

TEST_CASE("held states are on while the effect runs and off after", "[magic][archetype]") {
    Rig rig;
    rig.raw(k_player, k_skeever, k_src, k_calm, 99, 5);
    rig.raw(k_player, k_skeever, k_src, k_frenzy, 99, 5);
    rig.raw(k_player, k_skeever, k_src, k_paralysis, 0, 5);
    rig.raw(k_player, k_skeever, k_src, k_invis, 0, 5);
    rig.raw(k_player, k_skeever, k_src, k_demoralize, 99, 5);
    auto o = rig.effects.overrides(k_skeever);
    CHECK((o.calm && o.frenzy && o.paralyzed && o.invisible));
    CHECK(o.fear_from == k_player);
    CHECK(o.time_scale == Approx(1.0));
    const auto other = rig.effects.overrides(k_wolf);
    CHECK_FALSE((other.calm || other.frenzy || other.paralyzed || other.invisible));
    CHECK(other.fear_from == 0);
    rig.run(5.5);
    o = rig.effects.overrides(k_skeever);
    CHECK_FALSE((o.calm || o.frenzy || o.paralyzed || o.invisible));
    CHECK(o.fear_from == 0);
    CHECK(rig.effects.unhandled().empty());
}

TEST_CASE("fear comes from the caster of the newest fear", "[magic][archetype]") {
    Rig rig;
    rig.raw(k_player, k_skeever, k_src, k_demoralize, 99, 30);
    rig.raw(k_wolf, k_skeever, k_src2, k_turn, 99, 30);
    CHECK(rig.effects.overrides(k_skeever).fear_from == k_wolf);
}

TEST_CASE("acting breaks invisibility", "[magic][archetype]") {
    Rig rig;
    rig.raw(k_player, k_player, k_src, k_invis, 0, 60);
    REQUIRE(rig.effects.overrides(k_player).invisible);
    CHECK(rig.effects.break_invisibility(k_wolf) == 0);
    CHECK(rig.effects.break_invisibility(k_player) == 1);
    CHECK_FALSE(rig.effects.overrides(k_player).invisible);
    CHECK_FALSE(rig.effects.has_effect(k_player, k_invis));
}

TEST_CASE("stagger is instant and holds nothing", "[magic][archetype]") {
    Rig rig;
    CHECK(rig.raw(k_player, k_skeever, k_src, k_stagger, 3, 0).size() == 1);
    REQUIRE(rig.host.events.size() == 2);
    CHECK(rig.host.events[0] == std::make_pair(true, k_stagger));
    CHECK(rig.effects.size() == 0);
    CHECK(rig.effects.unhandled().empty());
}

TEST_CASE("a summon is placed, commanded by the caster and removed at the end", "[magic][archetype][summon]") {
    Rig rig;
    const auto ids = rig.raw(k_player, k_player, k_src, k_summon, 0, 10);
    REQUIRE(ids.size() == 1);
    REQUIRE(rig.host.placed.size() == 1);
    CHECK(rig.host.placed[0] == std::make_pair(k_player, k_npc_base));
    CHECK(rig.effects.instance(ids[0])->summoned == 0x900);
    CHECK(rig.effects.overrides(0x900).commanded_by == k_player);
    CHECK(rig.effects.overrides(k_wolf).commanded_by == 0);
    rig.run(10.5);
    CHECK(rig.host.removed == std::vector<std::uint32_t>{0x900});
    CHECK(rig.effects.overrides(0x900).commanded_by == 0);
}

TEST_CASE("a failed placement ends the summon at once", "[magic][archetype][summon]") {
    Rig rig;
    rig.host.fail_summon = true;
    CHECK(rig.raw(k_player, k_player, k_src, k_summon, 0, 10).empty());
    CHECK(rig.effects.size() == 0);
    CHECK(rig.host.removed.empty());
    CHECK(rig.host.events.empty());
}

TEST_CASE("a second summon by the same caster removes the first", "[magic][archetype][summon]") {
    Rig rig;
    rig.raw(k_player, k_player, k_src, k_summon, 0, 60);
    rig.raw(k_wolf, k_wolf, k_src, k_summon, 0, 60); // another caster's stays
    rig.raw(k_player, k_player, k_src, k_summon, 0, 60); // even the same spell again
    CHECK(rig.host.removed == std::vector<std::uint32_t>{0x900});
    CHECK(rig.effects.overrides(0x901).commanded_by == k_wolf);
    CHECK(rig.effects.overrides(0x902).commanded_by == k_player);
    CHECK(rig.effects.overrides(0x900).commanded_by == 0);
}

TEST_CASE("when the summon dies the effect ends", "[magic][archetype][summon]") {
    Rig rig;
    const auto ids = rig.raw(k_player, k_player, k_src, k_summon, 0, 60);
    rig.run(1.0);
    CHECK(rig.effects.instance(ids[0]).has_value());
    rig.host.dead_refs.insert(0x900);
    rig.run(0.2);
    CHECK_FALSE(rig.effects.instance(ids[0]).has_value());
    CHECK(rig.host.removed == std::vector<std::uint32_t>{0x900});
}

TEST_CASE("reanimate raises only the dead and lets them die at the end", "[magic][archetype][reanimate]") {
    Rig rig;
    CHECK(rig.raw(k_player, k_wolf, k_src, k_reanimate, 50, 20).empty()); // alive: nothing happens
    rig.host.states.damage(k_corpse, k_health, 1000);
    REQUIRE(rig.host.is_dead(k_corpse));
    rig.host.levels[k_corpse] = 60;
    CHECK(rig.raw(k_player, k_corpse, k_src, k_reanimate, 50, 20).empty()); // too strong
    rig.host.levels[k_corpse] = 10;
    REQUIRE(rig.raw(k_player, k_corpse, k_src, k_reanimate, 50, 20).size() == 1);
    CHECK_FALSE(rig.host.is_dead(k_corpse));
    CHECK(rig.effects.overrides(k_corpse).commanded_by == k_player);
    rig.effects.on_death(k_corpse); // a stale death notice does not end it while it stands
    CHECK(rig.effects.has_effect(k_corpse, k_reanimate));
    rig.run(20.5);
    CHECK(rig.host.ended_reanimate == std::vector<std::uint32_t>{k_corpse});
    CHECK(rig.host.is_dead(k_corpse));
    CHECK(rig.effects.overrides(k_corpse).commanded_by == 0);
}

TEST_CASE("a reanimated actor killed again ends the effect", "[magic][archetype][reanimate]") {
    Rig rig;
    rig.host.states.damage(k_corpse, k_health, 1000);
    rig.raw(k_player, k_corpse, k_src, k_reanimate, 50, 60);
    rig.host.states.damage(k_corpse, k_health, 1000);
    rig.effects.on_death(k_corpse);
    CHECK_FALSE(rig.effects.has_effect(k_corpse, k_reanimate));
    CHECK(rig.host.ended_reanimate.empty()); // already dead
}

TEST_CASE("a bound weapon is added and removed, or dispelled by the game", "[magic][archetype][bound]") {
    Rig rig;
    rig.raw(k_player, k_player, k_src, k_bound, 0, 5);
    REQUIRE(rig.host.bound.size() == 1);
    CHECK(rig.host.bound[0] == std::make_tuple(true, k_player, k_weapon));
    rig.run(5.5);
    REQUIRE(rig.host.bound.size() == 2);
    CHECK(rig.host.bound[1] == std::make_tuple(false, k_player, k_weapon));

    rig.raw(k_player, k_player, k_src, k_bound, 0, 60);
    CHECK(rig.effects.dispel_bound(k_player, 0x1234) == 0);
    CHECK(rig.effects.dispel_bound(k_player, k_weapon) == 1);
    CHECK(rig.host.bound.size() == 4);
    CHECK_FALSE(rig.effects.has_effect(k_player, k_bound));
}

TEST_CASE("a cloak applies its spell to those near every second, not to its bearer", "[magic][archetype][cloak]") {
    Rig rig;
    rig.host.states.damage(k_corpse, k_health, 1000);
    rig.host.near = {k_player, k_skeever, k_wolf, k_corpse};
    rig.raw(k_player, k_player, k_src, k_cloak, 0, 30);
    rig.run(0.8);
    CHECK(rig.host.damaged.empty());
    rig.run(0.5);
    CHECK(rig.host.damaged.contains(k_skeever));
    CHECK(rig.host.damaged.contains(k_wolf));
    CHECK_FALSE(rig.host.damaged.contains(k_player));
    CHECK_FALSE(rig.host.damaged.contains(k_corpse));
    CHECK(rig.host.last_radius == Approx(magic::area_units(10))); // PLACEHOLDER area
    const double health = rig.host.states.get(k_skeever, k_health);
    rig.run(1.0);
    CHECK(rig.host.states.get(k_skeever, k_health) < health); // and again
    CHECK(rig.effects.unhandled().empty());
}

TEST_CASE("slow time scales the world by its magnitude", "[magic][archetype][slow]") {
    CHECK(magic::slow_time_scale(0.3) == Approx(0.3));
    CHECK(magic::slow_time_scale(50.0) == Approx(0.5));
    CHECK(magic::slow_time_scale(100.0) == Approx(0.1)); // clamped
    CHECK(magic::slow_time_scale(0.0) == Approx(1.0));
    Rig rig;
    rig.raw(k_player, k_player, k_src, k_slow, 0.2F, 5);
    CHECK(rig.effects.overrides(k_player).time_scale == Approx(0.2));
    CHECK(rig.effects.overrides(k_wolf).time_scale == Approx(1.0));
    rig.run(5.5);
    CHECK(rig.effects.overrides(k_player).time_scale == Approx(1.0));
}

TEST_CASE("the section keeps a summon's ref (version 2) and reads version 1", "[magic][archetype][save]") {
    Rig rig;
    rig.raw(k_player, k_player, k_src, k_summon, 0, 60);
    rig.raw(k_player, k_skeever, k_src, k_calm, 99, 60);
    const auto bytes = rig.effects.save();
    CHECK(magic::ActiveEffects::check(*rig.world, skydot::save::ByteReader(bytes)));

    Rig other;
    REQUIRE(other.effects.load(skydot::save::ByteReader(bytes)));
    CHECK(other.host.placed.empty()); // nothing is placed again
    CHECK(other.effects.overrides(0x900).commanded_by == k_player);
    CHECK(other.effects.overrides(k_skeever).calm);
    CHECK(other.effects.dispel_all(k_player) == 1);
    CHECK(other.host.removed == std::vector<std::uint32_t>{0x900});

    // Version 1: no summoned ref at the end of a record.
    Rig old;
    old.raw(k_player, k_skeever, k_src, k_calm, 99, 60);
    auto v1 = old.effects.save();
    v1[0] = 1;
    v1.resize(v1.size() - 4);
    Rig loaded;
    REQUIRE(loaded.effects.load(skydot::save::ByteReader(v1)));
    CHECK(loaded.effects.overrides(k_skeever).calm);
    CHECK(loaded.effects.effects_on(k_skeever)[0].summoned == 0);
}
