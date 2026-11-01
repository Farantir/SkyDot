// SPDX-License-Identifier: GPL-3.0-or-later
//
// The combat domain module (vm/combat_runtime.cpp) over FakeActors: ghosts
// reported once, the player's death and restore, the hooks and the combat
// natives (with and without the AI's hooks).
#include "support/fake_hosts.hpp"
#include "support/native_calls.hpp"
#include "support/vm_rig.hpp"

#include "vm/combat_runtime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace skydot::testing;
using skydot::vm::CombatRuntime;
namespace av = skydot::vm::av;

namespace {

constexpr std::uint32_t k_player = 0x14;
constexpr std::uint32_t k_wolf = 0x40;

struct Host final : CombatRuntime::Host {
    std::vector<std::string> lines;
    void log(const std::string& text) override { lines.push_back(text); }
};

struct Fixture {
    Host host;
    FakeActors actors;
    Rig rig;
    CombatRuntime combat;
    Fixture() {
        actors.actors = {k_player, k_wolf};
        rig.add({.name = "Actor",
                 .functions = {native("IsInCombat", {}, "Bool"), native("GetCombatState", {}, "Int"),
                               native("GetCombatTarget", {}, "Actor"), native("StartCombat", {{"t", "Actor"}}),
                               native("StopCombat")}});
        rig.add(script("Caller", {wrap("incombat", "Actor", "IsInCombat", {}, "Bool"),
                                  wrap("state", "Actor", "GetCombatState", {}, "Int"),
                                  wrap("target", "Actor", "GetCombatTarget", {}, "Actor"),
                                  wrap("start", "Actor", "StartCombat", {{"t", "Actor"}}),
                                  wrap("stop", "Actor", "StopCombat")}));
        combat.bind_natives(rig.vm, actors, host);
    }
    skydot::vm::Value actor(std::uint32_t ref) { return rig.vm.object(ref, "Actor"); }
};

} // namespace

TEST_CASE("a ghost takes no hits and is reported once", "[combat][vm]") {
    Fixture f;
    CHECK_FALSE(f.combat.ghost_ignores_hit(k_wolf));
    f.actors.states.set_ghost(k_wolf, true);
    CHECK(f.combat.ghost_ignores_hit(k_wolf));
    CHECK(f.combat.ghost_ignores_hit(k_wolf));
    CHECK(f.host.lines.size() == 1);
    CHECK(f.combat.ghost_count() == 1);
    f.combat.forget_ghost(k_wolf);
    CHECK(f.combat.ghost_count() == 0);
    CHECK(f.combat.ghost_ignores_hit(k_wolf));
    CHECK(f.host.lines.size() == 2);
}

TEST_CASE("the player dies once, the dead do not regenerate, a restore brings him back", "[combat][vm]") {
    Fixture f;
    CHECK_FALSE(f.combat.check_player_death());
    f.combat.set_last_damager(k_wolf);
    f.actors.states.damage(k_player, av::k_health, 1000);
    CHECK(f.combat.check_player_death());
    CHECK(f.combat.player_dead());
    CHECK(f.host.lines == std::vector<std::string>{"death: player killed by 0x40"});
    CHECK_FALSE(f.combat.check_player_death()); // once
    f.actors.states.set_base(k_player, av::k_health, 50); // healed by something
    CHECK_FALSE(f.combat.check_player_death());
    CHECK(f.actors.states.get(k_player, av::k_health) <= 0.0); // and damaged away again
    f.combat.player_restored();
    CHECK_FALSE(f.combat.player_dead());
    CHECK(f.combat.last_damager() == 0);
}

TEST_CASE("the last damager is forgotten with its reference", "[combat][vm]") {
    CombatRuntime c;
    c.set_last_damager(k_wolf);
    CHECK(c.damager_count() == 1);
    c.forget_damager(0x99);
    CHECK(c.last_damager() == k_wolf);
    c.forget_damager(k_wolf);
    CHECK(c.last_damager() == 0);
    CHECK(c.damager_count() == 0);
}

TEST_CASE("without the AI's hooks nobody fights", "[combat][vm][natives]") {
    Fixture f;
    CHECK_FALSE(f.combat.in_combat(k_wolf));
    CHECK_FALSE(f.rig.call("caller", "incombat", {f.actor(k_wolf)}).b());
    CHECK(f.rig.call("caller", "state", {f.actor(k_wolf)}).i() == 0);
    f.rig.call("caller", "start", {f.actor(k_wolf), f.actor(k_player)}); // no hook: nothing happens
    f.rig.call("caller", "stop", {f.actor(k_wolf)});
}

TEST_CASE("the combat natives ask the hooks", "[combat][vm][natives]") {
    Fixture f;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> started;
    std::vector<std::uint32_t> stopped;
    skydot::vm::CombatHooks hooks;
    hooks.state = [](std::uint32_t ref) -> std::int64_t { return ref == k_wolf ? 1 : 0; };
    hooks.target = [](std::uint32_t ref) -> std::uint32_t { return ref == k_wolf ? k_player : 0; };
    hooks.start = [&](std::uint32_t ref, std::uint32_t target) { started.emplace_back(ref, target); };
    hooks.stop = [&](std::uint32_t ref) { stopped.push_back(ref); };
    f.combat.set_hooks(hooks);
    CHECK(f.combat.in_combat(k_wolf));
    CHECK_FALSE(f.combat.in_combat(k_player));
    CHECK(f.rig.call("caller", "incombat", {f.actor(k_wolf)}).b());
    CHECK(f.rig.call("caller", "state", {f.actor(k_wolf)}).i() == 1);
    CHECK(f.rig.call("caller", "target", {f.actor(k_wolf)}).form() == k_player);
    f.rig.call("caller", "start", {f.actor(k_player), f.actor(k_wolf)});
    f.rig.call("caller", "stop", {f.actor(k_wolf)});
    REQUIRE(started.size() == 1);
    CHECK(started[0] == std::pair<std::uint32_t, std::uint32_t>{k_player, k_wolf});
    CHECK(stopped == std::vector<std::uint32_t>{k_wolf});
}
