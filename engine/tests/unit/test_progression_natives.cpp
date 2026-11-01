// SPDX-License-Identifier: GPL-3.0-or-later
//
// The progression natives (vm/progression_runtime.cpp bind_natives) run from compiled scripts over a fake host:
// Game.AdvanceSkill and IncrementSkill(By) reach the host with the actor value, player experience and perk
// points, Actor.AddPerk, RemovePerk and HasPerk.
#include "support/script_builder.hpp"
#include "support/vm_rig.hpp"
#include "support/native_calls.hpp"

#include "vm/progression_runtime.hpp"
#include "world_generated.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace skydot::testing;
using skydot::vm::Op;
using skydot::vm::Value;
namespace vm = skydot::vm;

namespace {

constexpr std::uint32_t k_player = 0x14;
constexpr std::uint32_t k_perk = 0x500;

class FakeHost final : public vm::ProgressionRuntime::Host {
public:
    const bethconv::pack::wfb::World* world_root() const override { return nullptr; }
    std::uint32_t base_of(std::uint32_t) const override { return 0; }
    bool advance_skill(std::uint32_t av, double amount) override {
        advanced.emplace_back(av, amount);
        return handled;
    }
    bool increment_skill(std::uint32_t av, std::int32_t count) override {
        incremented.emplace_back(av, count);
        return handled;
    }
    double level_threshold(std::int32_t level) const override { return 10.0 * level; }
    void log(const std::string& text) override { lines.push_back(text); }
    void perk_gained(std::uint32_t ref, std::uint32_t perk) override { gained.emplace_back(ref, perk); }
    std::vector<std::pair<std::uint32_t, double>> advanced;
    std::vector<std::pair<std::uint32_t, std::int32_t>> incremented;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> gained;
    std::vector<std::string> lines;
    bool handled = true;
};

struct Fixture {
    FakeHost host;
    Rig rig;
    vm::ProgressionRuntime progression;
    Fixture() {
        rig.add({.name = "Game",
                 .functions = {native_function("AdvanceSkill", {{"s", "String"}, {"a", "Float"}}),
                               native_function("IncrementSkill", {{"s", "String"}}),
                               native_function("IncrementSkillBy", {{"s", "String"}, {"n", "Int"}}),
                               native_function("GetPlayerExperience", {}, "Float"),
                               native_function("SetPlayerExperience", {{"x", "Float"}}),
                               native_function("GetExperienceForLevel", {{"l", "Int"}}, "Float"),
                               native_function("GetPerkPoints", {}, "Int"), native_function("SetPerkPoints", {{"n", "Int"}}),
                               native_function("ModPerkPoints", {{"n", "Int"}}), native_function("AddPerkPoints", {{"n", "Int"}})}});
        rig.add({.name = "Form", .functions = {}});
        rig.add({.name = "Perk", .parent = "Form"});
        rig.add({.name = "Actor",
                 .parent = "Form",
                 .functions = {native("AddPerk", {{"p", "Perk"}}), native("RemovePerk", {{"p", "Perk"}}),
                               native("HasPerk", {{"p", "Perk"}}, "Bool")}});
        std::vector<FunctionSpec> fns;
        const auto stat = [&](const std::string& name, const std::string& fn, std::vector<Param> params, const std::string& ret) {
            std::vector<Param> locals;
            if (ret != "None") {
                locals.push_back({"r", ret});
            }
            auto f = global_function(name, params, locals, ret);
            std::vector<Arg> args{ident("Game"), ident(fn), ident(ret == "None" ? "::NoneVar" : "r")};
            for (const auto& p : params) {
                args.push_back(ident(p.name));
            }
            f.add(Op::callstatic, args);
            if (ret != "None") {
                f.add(Op::return_, {ident("r")});
            }
            fns.push_back(f);
        };
        stat("advance", "AdvanceSkill", {{"s", "String"}, {"a", "Float"}}, "None");
        stat("increment", "IncrementSkill", {{"s", "String"}}, "None");
        stat("incrementby", "IncrementSkillBy", {{"s", "String"}, {"n", "Int"}}, "None");
        stat("getxp", "GetPlayerExperience", {}, "Float");
        stat("setxp", "SetPlayerExperience", {{"x", "Float"}}, "None");
        stat("xpfor", "GetExperienceForLevel", {{"l", "Int"}}, "Float");
        stat("getpoints", "GetPerkPoints", {}, "Int");
        stat("setpoints", "SetPerkPoints", {{"n", "Int"}}, "None");
        stat("modpoints", "ModPerkPoints", {{"n", "Int"}}, "None");
        stat("addpoints", "AddPerkPoints", {{"n", "Int"}}, "None");
        fns.push_back(wrap("addperk", "Actor", "AddPerk", {{"p", "Perk"}}));
        fns.push_back(wrap("removeperk", "Actor", "RemovePerk", {{"p", "Perk"}}));
        fns.push_back(wrap("hasperk", "Actor", "HasPerk", {{"p", "Perk"}}, "Bool"));
        rig.add(script("Caller", fns));
        progression.bind(host);
        progression.bind_natives(rig.vm);
    }
    Value actor(std::uint32_t ref) { return rig.vm.object(ref, "Actor"); }
    Value perk(std::uint32_t id) { return rig.vm.object(id, "Perk"); }
};

} // namespace

TEST_CASE("Game.AdvanceSkill reaches the host with the actor value and the amount", "[progression][vm][natives]") {
    Fixture f;
    f.rig.call("caller", "advance", {Value::string("OneHanded"), Float(12.5F)});
    f.rig.call("caller", "advance", {Value::string("Marksman"), Float(1.0F)});
    f.rig.call("caller", "advance", {Value::string("archery"), Float(2.0F)});   // an alias
    f.rig.call("caller", "advance", {Value::string("Speechcraft"), Float(3.0F)});
    f.rig.call("caller", "advance", {Value::string("Health"), Float(9.0F)});    // not a skill
    f.rig.call("caller", "advance", {Value::string("Nonsense"), Float(9.0F)});
    REQUIRE(f.host.advanced.size() == 4);
    CHECK(f.host.advanced[0] == std::pair<std::uint32_t, double>{6, 12.5});
    CHECK(f.host.advanced[1].first == 8);
    CHECK(f.host.advanced[2].first == 8);
    CHECK(f.host.advanced[3].first == 17);
}

TEST_CASE("Game.IncrementSkill raises one level, IncrementSkillBy several", "[progression][vm][natives]") {
    Fixture f;
    f.rig.call("caller", "increment", {Value::string("Lockpicking")});
    f.rig.call("caller", "incrementby", {Value::string("Pickpocket"), Int(5)});
    REQUIRE(f.host.incremented.size() == 2);
    CHECK(f.host.incremented[0] == std::pair<std::uint32_t, std::int32_t>{14, 1});
    CHECK(f.host.incremented[1] == std::pair<std::uint32_t, std::int32_t>{13, 5});
}

TEST_CASE("with nobody to apply the skill rules the calls log once", "[progression][vm][natives]") {
    Fixture f;
    f.host.handled = false;
    f.rig.call("caller", "advance", {Value::string("Sneak"), Float(1.0F)});
    f.rig.call("caller", "increment", {Value::string("Sneak")});
    CHECK(f.host.lines.size() == 1);
}

TEST_CASE("player experience, the experience for a level and the perk points", "[progression][vm][natives]") {
    Fixture f;
    f.rig.call("caller", "setxp", {Float(42.0F)});
    CHECK(f.rig.call("caller", "getxp").f() == 42.0F);
    CHECK(f.progression.player().xp == 42.0);
    CHECK(f.rig.call("caller", "xpfor", {Int(7)}).f() == 70.0F);
    f.rig.call("caller", "setpoints", {Int(3)});
    f.rig.call("caller", "modpoints", {Int(2)});
    f.rig.call("caller", "addpoints", {Int(1)});
    CHECK(f.rig.call("caller", "getpoints").i() == 6);
    f.rig.call("caller", "modpoints", {Int(-100)});
    CHECK(f.rig.call("caller", "getpoints").i() == 0);
}

TEST_CASE("Actor.AddPerk, HasPerk and RemovePerk, without spending perk points", "[progression][vm][natives]") {
    Fixture f;
    f.progression.set_perk_points(2);
    CHECK_FALSE(f.rig.call("caller", "hasperk", {f.actor(k_player), f.perk(k_perk)}).b());
    f.rig.call("caller", "addperk", {f.actor(k_player), f.perk(k_perk)});
    CHECK(f.rig.call("caller", "hasperk", {f.actor(k_player), f.perk(k_perk)}).b());
    CHECK(f.progression.has_perk(k_player, k_perk));
    CHECK(f.progression.player().perk_points == 2);
    CHECK(f.host.gained == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{k_player, k_perk}});
    f.rig.call("caller", "removeperk", {f.actor(k_player), f.perk(k_perk)});
    CHECK_FALSE(f.rig.call("caller", "hasperk", {f.actor(k_player), f.perk(k_perk)}).b());
}
