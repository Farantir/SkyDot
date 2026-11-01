// SPDX-License-Identifier: GPL-3.0-or-later
//
// Quest, alias and global variable natives (vm/natives_quest.cpp), over
// FakeQuests and FakeWorld.
#include "support/fake_hosts.hpp"
#include "support/fake_world.hpp"
#include "support/vm_rig.hpp"

#include "vm/natives_args.hpp"
#include "vm/natives_families.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace skydot::testing;
using skydot::vm::Op;
using skydot::vm::Value;

namespace {

/// `Caller.<name>(o, extras...)`: o.<fn>(extras...) returning its result.
FunctionSpec wrap(const std::string& name, const std::string& cls, const std::string& fn,
                  std::vector<Param> extra = {}, const std::string& ret = "None") {
    std::vector<Param> params{{"o", cls}};
    params.insert(params.end(), extra.begin(), extra.end());
    std::vector<Param> locals;
    if (ret != "None") {
        locals.push_back({"r", ret});
    }
    auto f = global_function(name, std::move(params), std::move(locals), ret);
    std::vector<Arg> args{ident(fn), ident("o"), ident(ret == "None" ? "::NoneVar" : "r")};
    for (const auto& p : extra) {
        args.push_back(ident(p.name));
    }
    f.add(Op::callmethod, args);
    if (ret != "None") {
        f.add(Op::return_, {ident("r")});
    }
    return f;
}

FunctionSpec native(const std::string& name, std::vector<Param> params = {}, const std::string& ret = "None") {
    return native_function(name, std::move(params), ret, false);
}

void add_quest_classes(Rig& rig) {
    rig.add({.name = "Quest",
             .functions = {native("Start", {}, "Bool"), native("Stop"), native("Reset"), native("IsRunning", {}, "Bool"),
                           native("SetStage", {{"s", "Int"}}, "Bool"), native("GetStage", {}, "Int"),
                           native("GetStageDone", {{"s", "Int"}}, "Bool"), native("SetActive", {{"on", "Bool"}}),
                           native("IsActive", {}, "Bool"), native("GetAlias", {{"id", "Int"}}, "Alias"),
                           native("SetObjectiveDisplayed", {{"i", "Int"}, {"on", "Bool"}}),
                           native("IsObjectiveDisplayed", {{"i", "Int"}}, "Bool"), native("CompleteAllObjectives")}});
    rig.add({.name = "Alias",
             .functions = {native("GetName", {}, "String"), native("GetID", {}, "Int"),
                           native("GetOwningQuest", {}, "Quest")}});
    rig.add({.name = "ReferenceAlias",
             .parent = "Alias",
             .functions = {native("ForceRefTo", {{"r", "ObjectReference"}}),
                           native("ForceRefIfEmpty", {{"r", "ObjectReference"}}, "Bool"),
                           native("GetReference", {}, "ObjectReference"), native("Clear")}});
    rig.add({.name = "GlobalVariable",
             .functions = {native("GetValue", {}, "Float"), native("SetValue", {{"v", "Float"}})}});
    rig.add({.name = "TopicInfo", .functions = {native("GetOwningQuest", {}, "Quest")}});
    rig.add({.name = "Game", .functions = {native_function("GetFormFromFile", {{"id", "Int"}, {"p", "String"}}, "Form")}});
    rig.add({.name = "Form", .functions = {native("GetFormID", {}, "Int")}});
    rig.vm.bind("Form", "GetFormID", [](skydot::vm::NativeCall& c) {
        return skydot::vm::NativeResult{.value = Int(static_cast<std::int32_t>(c.self.form()))};
    });
    auto from_file = global_function("fromfile", {{"id", "Int"}, {"p", "String"}}, {{"f", "Form"}, {"r", "Int"}}, "Int");
    from_file.add(Op::callstatic, {ident("Game"), ident("GetFormFromFile"), ident("f"), ident("id"), ident("p")})
        .add(Op::callmethod, {ident("GetFormID"), ident("f"), ident("r")})
        .add(Op::return_, {ident("r")});
    rig.add(script("Caller",
                   {wrap("start", "Quest", "Start", {}, "Bool"), wrap("stop", "Quest", "Stop"),
                    wrap("reset", "Quest", "Reset"), wrap("running", "Quest", "IsRunning", {}, "Bool"),
                    wrap("setstage", "Quest", "SetStage", {{"s", "Int"}}, "Bool"),
                    wrap("getstage", "Quest", "GetStage", {}, "Int"),
                    wrap("stagedone", "Quest", "GetStageDone", {{"s", "Int"}}, "Bool"),
                    wrap("setactive", "Quest", "SetActive", {{"on", "Bool"}}),
                    wrap("isactive", "Quest", "IsActive", {}, "Bool"),
                    wrap("display", "Quest", "SetObjectiveDisplayed", {{"i", "Int"}, {"on", "Bool"}}),
                    wrap("displayed", "Quest", "IsObjectiveDisplayed", {{"i", "Int"}}, "Bool"),
                    wrap("completeall", "Quest", "CompleteAllObjectives"),
                    wrap("alias", "Quest", "GetAlias", {{"id", "Int"}}, "Alias"),
                    wrap("aliasname", "Alias", "GetName", {}, "String"), wrap("aliasid", "Alias", "GetID", {}, "Int"),
                    wrap("aliasquest", "Alias", "GetOwningQuest", {}, "Quest"),
                    wrap("forceref", "ReferenceAlias", "ForceRefTo", {{"r", "ObjectReference"}}),
                    wrap("forceifempty", "ReferenceAlias", "ForceRefIfEmpty", {{"r", "ObjectReference"}}, "Bool"),
                    wrap("getref", "ReferenceAlias", "GetReference", {}, "ObjectReference"),
                    wrap("clear", "ReferenceAlias", "Clear"),
                    wrap("getglobal", "GlobalVariable", "GetValue", {}, "Float"),
                    wrap("setglobal", "GlobalVariable", "SetValue", {{"v", "Float"}}),
                    wrap("infoquest", "TopicInfo", "GetOwningQuest", {}, "Quest"), from_file}));
}

struct Fixture {
    FakeWorld world;
    FakeQuests host;
    Rig rig;
    Fixture() {
        add_quest_classes(rig);
        skydot::vm::bind_quest_natives(rig.vm, world, host);
    }
    Value quest(std::uint32_t form) { return rig.vm.object(form, "Quest"); }
};

} // namespace

TEST_CASE("Quest.Start, Stop and Reset go through the host; Reset keeps only 'has started'", "[vm][natives]") {
    Fixture f;
    CHECK(f.rig.call("caller", "start", {f.quest(0x100)}).b());
    CHECK(f.rig.call("caller", "running", {f.quest(0x100)}).b());
    f.rig.call("caller", "setstage", {f.quest(0x100), Int(20)});
    CHECK(f.rig.call("caller", "getstage", {f.quest(0x100)}).i() == 20);
    CHECK(f.rig.call("caller", "stagedone", {f.quest(0x100), Int(20)}).b());
    CHECK_FALSE(f.rig.call("caller", "stagedone", {f.quest(0x100), Int(30)}).b());

    f.rig.call("caller", "reset", {f.quest(0x100)});
    REQUIRE(f.host.stopped.size() == 1);
    CHECK(f.host.stopped[0] == 0x100);
    const auto* state = f.host.states.find(0x100);
    REQUIRE(state != nullptr);
    CHECK(state->started);
    CHECK_FALSE(state->running);
    CHECK(state->stage == 0);
    CHECK(state->done.empty());

    f.rig.call("caller", "stop", {f.quest(0x100)});
    CHECK(f.host.stopped.size() == 2);
}

TEST_CASE("SetStage gives what the host says; SetActive defaults to true", "[vm][natives]") {
    Fixture f;
    f.host.refused_stages = {99};
    CHECK(f.rig.call("caller", "setstage", {f.quest(0x100), Int(10)}).b());
    CHECK_FALSE(f.rig.call("caller", "setstage", {f.quest(0x100), Int(99)}).b());
    CHECK(f.rig.call("caller", "getstage", {f.quest(0x100)}).i() == 10);

    // The script passes the flag; a call without one (fewer arguments) means true.
    f.rig.call("caller", "setactive", {f.quest(0x100), Bool(false)});
    CHECK_FALSE(f.rig.call("caller", "isactive", {f.quest(0x100)}).b());
    f.rig.call("caller", "setactive", {f.quest(0x100), Bool(true)});
    CHECK(f.rig.call("caller", "isactive", {f.quest(0x100)}).b());
}

TEST_CASE("objectives: the bit and flag are passed to the host, CompleteAll touches the displayed ones",
          "[vm][natives]") {
    Fixture f;
    f.rig.call("caller", "display", {f.quest(0x100), Int(10), Bool(true)});
    REQUIRE(f.host.objectives.size() == 1);
    CHECK(f.host.objectives[0].quest == 0x100);
    CHECK(f.host.objectives[0].index == 10);
    CHECK(f.host.objectives[0].bit == skydot::natives::k_displayed);
    CHECK(f.host.objectives[0].on);

    auto& bits = f.host.states.state(0x100).objectives;
    bits[10] = skydot::natives::k_displayed;
    bits[20] = 0;
    bits[30] = skydot::natives::k_displayed;
    CHECK(f.rig.call("caller", "displayed", {f.quest(0x100), Int(10)}).b());
    CHECK_FALSE(f.rig.call("caller", "displayed", {f.quest(0x100), Int(20)}).b());
    f.host.objectives.clear();
    f.rig.call("caller", "completeall", {f.quest(0x100)});
    REQUIRE(f.host.objectives.size() == 2);
    CHECK(f.host.objectives[0].index == 10);
    CHECK(f.host.objectives[1].index == 30);
    CHECK(f.host.objectives[0].bit == skydot::natives::k_completed);
}

TEST_CASE("Quest.GetAlias finds the alias in the data and gives a handle of the right class", "[vm][natives]") {
    Fixture f;
    f.host.aliases[0x100] = {{3, {.name = "Boss", .location = false}}, {4, {.name = "Home", .location = true}}};

    const auto boss = f.rig.call("caller", "alias", {f.quest(0x100), Int(3)});
    REQUIRE(boss.kind() == skydot::vm::Kind::object);
    CHECK(boss.form() == f.host.states.find_handle(0x100, 3));
    CHECK(f.rig.call("caller", "aliasname", {boss}).s() == "Boss");
    CHECK(f.rig.call("caller", "aliasid", {boss}).i() == 3);
    CHECK(f.rig.call("caller", "aliasquest", {boss}).form() == 0x100);

    const auto home = f.rig.call("caller", "alias", {f.quest(0x100), Int(4)});
    CHECK(home.form() == f.host.states.find_handle(0x100, 4));

    // An alias the quest has not: None; an alias handle nobody owns has no name and id -1.
    CHECK(f.rig.call("caller", "alias", {f.quest(0x100), Int(9)}).kind() == skydot::vm::Kind::none);
    const auto stranger = f.rig.vm.object(0x7777, "ReferenceAlias");
    CHECK(f.rig.call("caller", "aliasname", {stranger}).s().empty());
    CHECK(f.rig.call("caller", "aliasid", {stranger}).i() == -1);
}

TEST_CASE("a reference alias holds what it was forced to; ForceRefIfEmpty only fills an empty one", "[vm][natives]") {
    Fixture f;
    f.host.aliases[0x100] = {{3, {.name = "Boss", .location = false}}};
    const auto alias = f.rig.vm.object(f.host.alias_handle(0x100, 3), "ReferenceAlias");
    const auto ref = [&](std::uint32_t form) { return f.rig.vm.object(form, "ObjectReference"); };

    CHECK(f.rig.call("caller", "getref", {alias}).kind() == skydot::vm::Kind::none);
    CHECK(f.rig.call("caller", "forceifempty", {alias, ref(0x20)}).b());
    CHECK(f.rig.call("caller", "getref", {alias}).form() == 0x20);
    CHECK_FALSE(f.rig.call("caller", "forceifempty", {alias, ref(0x21)}).b());
    CHECK(f.rig.call("caller", "getref", {alias}).form() == 0x20);
    f.rig.call("caller", "forceref", {alias, ref(0x22)});
    CHECK(f.rig.call("caller", "getref", {alias}).form() == 0x22);
    f.rig.call("caller", "clear", {alias});
    CHECK(f.rig.call("caller", "getref", {alias}).kind() == skydot::vm::Kind::none);
    // Forcing None empties it.
    f.rig.call("caller", "forceref", {alias, ref(0x22)});
    f.rig.call("caller", "forceref", {alias, Value()});
    CHECK(f.rig.call("caller", "getref", {alias}).kind() == skydot::vm::Kind::none);
}

TEST_CASE("global variables read and write the host's values as floats; ints are accepted", "[vm][natives]") {
    Fixture f;
    f.host.globals[0x300] = 2.5;
    const auto global = f.rig.vm.object(0x300, "GlobalVariable");
    CHECK(f.rig.call("caller", "getglobal", {global}).f() == 2.5F);
    f.rig.call("caller", "setglobal", {global, Float(7.25F)});
    CHECK(f.host.globals[0x300] == 7.25);
}

TEST_CASE("TopicInfo.GetOwningQuest and Game.GetFormFromFile ask the host and the world", "[vm][natives]") {
    Fixture f;
    f.host.infos[0x400] = 0x100;
    CHECK(f.rig.call("caller", "infoquest", {f.rig.vm.object(0x400, "TopicInfo")}).form() == 0x100);
    CHECK(f.rig.call("caller", "infoquest", {f.rig.vm.object(0x401, "TopicInfo")}).kind() == skydot::vm::Kind::none);

    f.world.file_forms["Mod.esp:2049"] = 0x2000801;
    CHECK(f.rig.call("caller", "fromfile", {Int(2049), String("Mod.esp")}).i() == 0x2000801);
    REQUIRE(f.host.attached.size() == 1);
    CHECK(f.host.attached[0].quest == 0x2000801);
    CHECK(f.host.attached[0].on_init);
    // Not found: None, nothing attached (the script's error follows from None.GetFormID).
    f.rig.call("caller", "fromfile", {Int(1), String("Mod.esp")});
    CHECK(f.host.attached.size() == 1);
}
