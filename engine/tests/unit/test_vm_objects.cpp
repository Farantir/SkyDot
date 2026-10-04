// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the Papyrus VM does with scripts and the objects they are attached to
// (vm/vm.cpp, vm/script_class.cpp): instances, properties, inheritance and
// dispatch, states, events, natives, and loading classes and script assets.
// docs/papyrus.md, "Objects and dispatch" and "Natives".
#include "support/vm_rig.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace skydot::testing;
using skydot::vm::Kind;
using skydot::vm::Op;
using skydot::vm::Value;
using Catch::Matchers::ContainsSubstring;

// ---- instances, properties, inheritance, states -----------------------------

TEST_CASE("an attached script starts with its variables, its auto state and no OnBeginState", "[vm][instances]") {
    ScriptSpec spec = script("Fresh",
                             {method("OnBeginState", {}, {}, "None", "idle")},
                             {{"count", "Int", integer(5)},
                              {"ratio", "Float", integer(2)}, // an Int literal, kept as the Float it is declared as
                              {"label", "String", str("box")},
                              {"armed", "Bool", boolean(true)},
                              {"plain", "Int"}, // no initial value: the type's default
                              {"text", "String"},
                              {"target", "ObjectReference"}});
    spec.auto_state = "Idle";
    Rig rig;
    rig.add(spec);

    auto* instance = rig.vm.attach(0x40, "FRESH");
    REQUIRE(instance != nullptr);
    CHECK(instance->form == 0x40);
    CHECK(instance->state == "idle");
    CHECK(rig.vm.instance(0x40, "fresh") == instance);
    CHECK(rig.vm.instances(0x40) == std::vector{instance});
    CHECK(rig.vm.instances(0x41).empty());

    CHECK(rig.vm.get_variable(instance, "count").i == 5);
    const auto ratio = rig.vm.get_variable(instance, "RATIO");
    CHECK(ratio.kind == Kind::floating);
    CHECK(ratio.f == 2.0F);
    CHECK(rig.vm.get_variable(instance, "label").s == "box");
    CHECK(rig.vm.get_variable(instance, "armed").b);
    const auto plain = rig.vm.get_variable(instance, "plain");
    CHECK(plain.kind == Kind::integer);
    CHECK(plain.i == 0);
    CHECK(rig.vm.get_variable(instance, "text").kind == Kind::string);
    CHECK(rig.vm.get_variable(instance, "target").is_none());
    CHECK(rig.vm.get_variable(instance, "nosuchvariable").is_none());

    // The auto state's OnBeginState is not sent on attach (docs/papyrus.md).
    CHECK(rig.vm.thread_count() == 0);
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("only scripts can be attached: not engine types, not scripts the pack lacks", "[vm][instances]") {
    Rig rig;
    CHECK(rig.vm.attach(0x40, "Util") == nullptr); // declares natives
    REQUIRE(rig.log.size() == 1);
    CHECK_THAT(rig.log[0], ContainsSubstring("engine type"));
    CHECK(rig.vm.attach(0x40, "NoSuchScript") == nullptr);
    CHECK(rig.vm.errors() == 2);
    CHECK(rig.vm.instances(0x40).empty());
}

TEST_CASE("properties: auto ones are variables, others run their getter and setter", "[vm][properties]") {
    // Function Doubled.get(): half * 2; Doubled.set(int value): half = value / 2
    FunctionSpec getter{.name = "Doubled", .return_type = "Int", .locals = {{"r", "Int"}}};
    getter.add(Op::imul, {ident("r"), ident("half"), integer(2)}).add(Op::return_, {ident("r")});
    FunctionSpec setter{.name = "Doubled", .params = {{"value", "Int"}}};
    setter.add(Op::idiv, {ident("half"), ident("value"), integer(2)});

    // Function Bump(): Counter += 1, through the property
    auto bump = method("Bump", {}, {{"t", "Int"}});
    bump.add(Op::propget, {ident("Counter"), ident("self"), ident("t")})
        .add(Op::iadd, {ident("t"), ident("t"), integer(1)})
        .add(Op::propset, {ident("Counter"), ident("self"), ident("t")});
    auto read = method("ReadDoubled", {}, {{"r", "Int"}}, "Int");
    read.add(Op::propget, {ident("Doubled"), ident("self"), ident("r")}).add(Op::return_, {ident("r")});
    auto write = method("WriteDoubled", {{"v", "Int"}});
    write.add(Op::propset, {ident("Doubled"), ident("self"), ident("v")});

    ScriptSpec spec = script("Props", {bump, read, write}, {{"::Counter_var", "Int", integer(5)}, {"half", "Int"}});
    spec.properties = {{.name = "Counter", .type = "Int", .auto_var = "::Counter_var"},
                       {.name = "Doubled", .type = "Int", .auto_var = {}, .getter = getter, .setter = setter}};
    Rig rig;
    rig.add(spec);
    auto* instance = rig.vm.attach(0x40, "props");
    REQUIRE(instance != nullptr);

    // A script reads and writes an auto property as its variable.
    rig.call(instance, "bump");
    CHECK(rig.vm.get_variable(instance, "::counter_var").i == 6);

    // Set the way VMAD does: an auto property's variable directly, converted
    // to the property's type.
    CHECK(rig.vm.set_property(instance, "COUNTER", Float(7.9F)));
    const auto counter = rig.vm.get_variable(instance, "::counter_var");
    CHECK(counter.kind == Kind::integer);
    CHECK(counter.i == 7);

    // Any other property goes through its setter, on a thread of its own.
    CHECK(rig.vm.set_property(instance, "doubled", Int(10)));
    CHECK(rig.vm.get_variable(instance, "half").i == 0);
    rig.vm.update(0.0);
    CHECK(rig.vm.get_variable(instance, "half").i == 5);
    CHECK(rig.call(instance, "readdoubled").i == 10);
    rig.call(instance, "writedoubled", {Int(40)});
    CHECK(rig.vm.get_variable(instance, "half").i == 20);

    CHECK_FALSE(rig.vm.set_property(instance, "nosuchproperty", Int(1)));
    CHECK(rig.vm.errors() == 0);
}

namespace {

/// Animal with Speak and Describe (which calls Speak), and Dog extends Animal
/// overriding Speak with a call to its parent's.
void add_zoo(Rig& rig) {
    auto speak = method("Speak", {}, {}, "String");
    speak.add(Op::return_, {str("animal")});
    auto describe = method("Describe", {}, {{"r", "String"}}, "String");
    describe.add(Op::callmethod, {ident("Speak"), ident("self"), ident("r")}).add(Op::return_, {ident("r")});
    rig.add(script("Animal", {speak, describe}, {{"legs", "Int", integer(4)}}));

    auto dog_speak = method("Speak", {}, {{"r", "String"}}, "String");
    dog_speak.add(Op::callparent, {ident("Speak"), ident("r")})
        .add(Op::strcat, {ident("r"), ident("r"), str(" woof")})
        .add(Op::return_, {ident("r")});
    ScriptSpec dog = script("Dog", {dog_speak}, {{"tricks", "Int", integer(2)}});
    dog.parent = "Animal";
    rig.add(dog);

    // Function Introduce(Animal a) global: a.Speak()
    auto introduce = global_function("introduce", {{"a", "Animal"}}, {{"r", "String"}}, "String");
    introduce.add(Op::callmethod, {ident("Speak"), ident("a"), ident("r")}).add(Op::return_, {ident("r")});
    rig.add(script("Zoo", {introduce}));
}

} // namespace

TEST_CASE("a script extends another: variables per class, overrides, callparent", "[vm][dispatch]") {
    Rig rig;
    add_zoo(rig);
    auto* dog = rig.vm.attach(0x50, "dog");
    REQUIRE(dog != nullptr);

    // The instance holds the variables of every class in its chain.
    CHECK(dog->vars.size() == 2);
    CHECK(rig.vm.get_variable(dog, "tricks").i == 2);
    CHECK(rig.vm.get_variable(dog, "legs").i == 4);
    CHECK(rig.vm.load_class("dog")->parent() == rig.vm.load_class("animal"));
    CHECK(rig.vm.load_class("dog")->derives_from(rig.vm.load_class("animal")));
    CHECK_FALSE(rig.vm.load_class("animal")->derives_from(rig.vm.load_class("dog")));

    // The override wins; callparent reaches the original; a method only the
    // parent has still calls the override of its own class's method.
    CHECK(rig.call(dog, "speak").s == "animal woof");
    CHECK(rig.call(dog, "describe").s == "animal woof");
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("a call on an object runs the attached script derived from the class it is seen as", "[vm][dispatch]") {
    Rig rig;
    add_zoo(rig);
    REQUIRE(rig.vm.attach(0x50, "dog") != nullptr);
    REQUIRE(rig.vm.attach(0x51, "animal") != nullptr);
    const auto seen_as_animal = [&](std::uint32_t form) { return rig.vm.object(form, "Animal"); };

    // A Dog seen as an Animal still barks (and the parameter's type keeps it).
    CHECK(rig.call("zoo", "introduce", {seen_as_animal(0x50)}).s == "animal woof");
    CHECK(rig.call("zoo", "introduce", {seen_as_animal(0x51)}).s == "animal");
    // Nothing attached: the class's own function.
    CHECK(rig.call("zoo", "introduce", {seen_as_animal(0x52)}).s == "animal");
    CHECK(rig.vm.errors() == 0);

    // Casting to a script type finds the attached instance or gives None;
    // casting to an engine type keeps the form.
    const auto dog = rig.vm.cast(seen_as_animal(0x50), "dog");
    CHECK(dog.kind == Kind::object);
    CHECK(dog.form == 0x50);
    CHECK(dog.instance == rig.vm.instance(0x50, "dog"));
    CHECK(rig.vm.cast(seen_as_animal(0x51), "dog").is_none());
    CHECK(rig.vm.cast(seen_as_animal(0x52), "dog").is_none());
    const auto reference = rig.vm.cast(seen_as_animal(0x52), "objectreference");
    CHECK(reference.kind == Kind::object);
    CHECK(reference.form == 0x52);
}

namespace {

/// Door: GotoState compiled the way the Papyrus compiler writes it, a
/// function per state, and handlers that note what happens.
ScriptSpec door() {
    // Function GotoState(string newState): OnEndState(newState), ::State = newState, OnBeginState(old)
    auto goto_state = method("GotoState", {{"newState", "String"}}, {{"old", "String"}});
    goto_state.add(Op::callmethod, {ident("GetState"), ident("self"), ident("old")})
        .add(Op::callmethod, {ident("OnEndState"), ident("self"), ident("::NoneVar"), ident("newState")})
        .add(Op::assign, {ident("::State"), ident("newState")})
        .add(Op::callmethod, {ident("OnBeginState"), ident("self"), ident("::NoneVar"), ident("old")});
    auto get_state = method("GetState", {}, {}, "String");
    get_state.add(Op::return_, {ident("::State")});

    auto name_in = [](const std::string& state, const std::string& text) {
        auto f = method("Describe", {}, {}, "String", state);
        f.add(Op::return_, {str(text)});
        return f;
    };
    auto common = method("Common", {}, {}, "Int");
    common.add(Op::return_, {integer(1)});

    std::vector<FunctionSpec> functions{goto_state, get_state, common, name_in("", "default"), name_in("idle", "idle"),
                                        name_in("busy", "busy")};
    for (const std::string state : {"idle", "busy"}) {
        auto begin = method("OnBeginState", {{"oldState", "String"}}, {{"t", "String"}}, "None", state);
        begin.add(Op::strcat, {ident("t"), str("begin " + state + " from "), ident("oldState")});
        call_static(begin, "Util", "Note", {ident("t")});
        auto end = method("OnEndState", {{"newState", "String"}}, {{"t", "String"}}, "None", state);
        end.add(Op::strcat, {ident("t"), str("end " + state + " for "), ident("newState")});
        call_static(end, "Util", "Note", {ident("t")});
        auto hit = method("OnHit", {}, {}, "None", state);
        note(hit, "hit while " + state);
        functions.push_back(begin);
        functions.push_back(end);
        functions.push_back(hit);
    }
    ScriptSpec spec = script("Door", functions);
    spec.auto_state = "idle";
    return spec;
}

} // namespace

TEST_CASE("states: ::State and GotoState change which function runs", "[vm][states]") {
    Rig rig;
    rig.add(door());
    auto* instance = rig.vm.attach(0x60, "door");
    REQUIRE(instance != nullptr);

    // Attached in the auto state, whose functions run.
    CHECK(instance->state == "idle");
    CHECK(rig.call(instance, "describe").s == "idle");
    CHECK(rig.call(instance, "getstate").s == "idle");
    CHECK(rig.notes.empty());

    // OnEndState goes to the state being left, OnBeginState to the new one.
    rig.call(instance, "gotostate", {String("busy")});
    CHECK(instance->state == "busy");
    CHECK(rig.notes == std::vector<std::string>{"end idle for busy", "begin busy from idle"});
    CHECK(rig.call(instance, "describe").s == "busy");

    // A function the state lacks is the default state's.
    CHECK(rig.call(instance, "common").i == 1);

    // The default state is the empty name; it has no state handlers, and a
    // missing On... function is no error.
    rig.notes.clear();
    rig.call(instance, "gotostate", {String("")});
    CHECK(instance->state.empty());
    CHECK(rig.notes == std::vector<std::string>{"end busy for "});
    CHECK(rig.call(instance, "describe").s == "default");
    CHECK(rig.vm.errors() == 0);

    // State names are case-insensitive.
    rig.call(instance, "gotostate", {String("IDLE")});
    CHECK(rig.call(instance, "describe").s == "idle");
}

TEST_CASE("states decide which events an instance handles", "[vm][states][events]") {
    Rig rig;
    rig.add(door());
    auto* instance = rig.vm.attach(0x60, "door");
    REQUIRE(instance != nullptr);

    CHECK(rig.vm.send_event(0x60, "OnHit", {}) == 1);
    rig.vm.update(0.0);
    CHECK(rig.notes == std::vector<std::string>{"hit while idle"});

    rig.call(instance, "gotostate", {String("busy")});
    rig.notes.clear();
    CHECK(rig.vm.send_event(0x60, "OnHit", {}) == 1);
    rig.vm.update(0.0);
    CHECK(rig.notes == std::vector<std::string>{"hit while busy"});

    // The default state has no OnHit: nothing starts.
    rig.call(instance, "gotostate", {String("")});
    CHECK(rig.vm.send_event(0x60, "OnHit", {}) == 0);
    CHECK(rig.vm.thread_count() == 0);
}

// ---- events -----------------------------------------------------------------

TEST_CASE("an event starts one thread per attached script that handles it", "[vm][events]") {
    // Alpha.OnActivate(ObjectReference akActionRef): notes "alpha <id>"
    auto alpha = method("OnActivate", {{"akActionRef", "ObjectReference"}}, {{"id", "Int"}, {"t", "String"}});
    alpha.add(Op::callmethod, {ident("GetFormID"), ident("akActionRef"), ident("id")})
        .add(Op::strcat, {ident("t"), str("alpha "), ident("id")});
    call_static(alpha, "Util", "Note", {ident("t")});
    auto beta = method("OnActivate");
    note(beta, "beta");
    auto gamma = method("OnHit");
    note(gamma, "gamma");

    Rig rig;
    rig.add(script("Alpha", {alpha}));
    rig.add(script("Beta", {beta}));
    rig.add(script("Gamma", {gamma}));
    ScriptSpec child = script("Child", {});
    child.parent = "Alpha";
    rig.add(child);
    REQUIRE(rig.vm.attach(0x70, "alpha") != nullptr);
    REQUIRE(rig.vm.attach(0x70, "beta") != nullptr);
    REQUIRE(rig.vm.attach(0x70, "gamma") != nullptr);
    REQUIRE(rig.vm.attach(0x71, "child") != nullptr);

    // Alpha and Beta handle it, Gamma does not; the argument reaches the
    // handler that declares it.
    const auto player = rig.vm.object(0x14, "ObjectReference");
    CHECK(rig.vm.send_event(0x70, "OnActivate", {player}) == 2);
    CHECK(rig.vm.thread_count() == 2);
    rig.vm.update(0.0);
    CHECK(rig.notes == std::vector<std::string>{"alpha 20", "beta"});
    CHECK(rig.vm.thread_count() == 0);

    // Event names are case-insensitive, and a handler is inherited.
    rig.notes.clear();
    CHECK(rig.vm.send_event(0x71, "onactivate", {player}) == 1);
    rig.vm.update(0.0);
    CHECK(rig.notes == std::vector<std::string>{"alpha 20"});

    // No handler, or no script on the form: nothing starts, nothing is wrong.
    CHECK(rig.vm.send_event(0x70, "OnNothing", {}) == 0);
    CHECK(rig.vm.send_event(0x99, "OnActivate", {player}) == 0);
    CHECK(rig.vm.errors() == 0);
}

// ---- natives ----------------------------------------------------------------

TEST_CASE("a bound native receives its arguments as its parameter types and returns into the caller", "[vm][natives]") {
    // Function Run() global: two Echo calls with arguments of other types than
    // the parameters', then Triple(14)
    auto run = global_function("run", {}, {{"t", "Int"}}, "Int");
    call_static(run, "Util", "Echo", {str("hi"), integer(3), integer(4), boolean(true)});
    call_static(run, "Util", "Echo", {integer(7), floating(2.9F), integer(5), integer(0)});
    run.add(Op::callstatic, {ident("Util"), ident("Triple"), ident("t"), integer(14)}).add(Op::return_, {ident("t")});
    Rig rig;
    rig.add(script("Natives", {run}));

    CHECK(rig.call("natives", "run").i == 42);
    REQUIRE(rig.echoes.size() == 2);
    REQUIRE(rig.echoes[0].size() == 4);
    CHECK(rig.echoes[0][0].kind == Kind::string);
    CHECK(rig.echoes[0][0].s == "hi");
    CHECK(rig.echoes[0][1].i == 3);
    CHECK(rig.echoes[0][2].kind == Kind::floating); // the Int 4 became a Float
    CHECK(rig.echoes[0][2].f == 4.0F);
    CHECK(rig.echoes[0][3].kind == Kind::boolean);
    CHECK(rig.echoes[0][3].b);
    CHECK(rig.echoes[1][0].kind == Kind::string);
    CHECK(rig.echoes[1][0].s == "7");
    CHECK(rig.echoes[1][1].kind == Kind::integer);
    CHECK(rig.echoes[1][1].i == 2);
    CHECK(rig.echoes[1][2].f == 5.0F);
    CHECK_FALSE(rig.echoes[1][3].b);
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("a native method is given the object it was called on", "[vm][natives]") {
    // Function Id(): GetFormID() of self
    auto id = method("Id", {}, {{"r", "Int"}}, "Int");
    id.add(Op::callmethod, {ident("GetFormID"), ident("self"), ident("r")}).add(Op::return_, {ident("r")});
    // Function Probe(ObjectReference o) global: o.GetFormID()
    auto probe = global_function("probe", {{"o", "ObjectReference"}}, {{"r", "Int"}}, "Int");
    probe.add(Op::callmethod, {ident("GetFormID"), ident("o"), ident("r")}).add(Op::return_, {ident("r")});

    Rig rig;
    ScriptSpec reference = script("Ref", {id});
    reference.parent = "ObjectReference";
    rig.add(reference);
    rig.add(script("Prober", {probe}));

    auto* instance = rig.vm.attach(0x1234, "ref");
    REQUIRE(instance != nullptr);
    CHECK(rig.call(instance, "id").i == 0x1234);
    CHECK(rig.call("prober", "probe", {rig.vm.object(0x77, "ObjectReference")}).i == 0x77);
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("a native nobody bound is logged once and gives None; the script goes on", "[vm][natives][errors]") {
    Rig rig;
    rig.add({.name = "Gaps", .functions = {native_function("Missing", {}, "Int")}});
    // Function Run() global: Missing() twice, then a note
    auto run = global_function("run", {}, {{"r", "Int"}}, "Int");
    run.add(Op::callstatic, {ident("Gaps"), ident("Missing"), ident("r")})
        .add(Op::callstatic, {ident("Gaps"), ident("Missing"), ident("r")});
    note(run, "went on");
    run.add(Op::return_, {ident("r")});
    rig.add(script("Caller", {run}));

    CHECK(rig.call("caller", "run").is_none());
    CHECK(rig.notes == std::vector<std::string>{"went on"});
    CHECK(rig.vm.errors() == 1);
    REQUIRE(rig.log.size() == 1);
    CHECK_THAT(rig.log[0], ContainsSubstring("not bound"));
    CHECK_THAT(rig.log[0], ContainsSubstring("Gaps"));
}

// ---- loading classes --------------------------------------------------------

TEST_CASE("classes load by name on first use, once, whatever the case", "[vm][loader]") {
    Rig rig;
    add_zoo(rig);
    const auto* animal = rig.vm.load_class("Animal");
    REQUIRE(animal != nullptr);
    CHECK(animal->name() == "Animal");
    CHECK(animal->key() == "animal");
    CHECK(rig.vm.load_class("ANIMAL") == animal);
    // Loading a class loads its parents first.
    const auto* dog = rig.vm.load_class("dog");
    REQUIRE(dog != nullptr);
    CHECK(dog->parent() == animal);
    CHECK(dog->parent_key() == "animal");
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("a class the pack lacks or cannot read is refused once, with a reason", "[vm][loader][errors]") {
    Rig rig;
    rig.scripts["junk"] = {1, 2, 3, 4};
    rig.scripts["misfiled"] = build_script(script("SomethingElse", {}));
    ScriptSpec orphan = script("Orphan", {});
    orphan.parent = "Ghost";
    rig.add(orphan);
    ScriptSpec loop = script("Loop", {});
    loop.parent = "Loop";
    rig.add(loop);
    ScriptSpec future = script("Future", {});
    future.format_version = 2;
    rig.add(future);

    CHECK(rig.vm.load_class("") == nullptr);
    CHECK(rig.vm.errors() == 0);

    CHECK(rig.vm.load_class("ghost") == nullptr);
    REQUIRE(rig.log.size() == 1);
    CHECK_THAT(rig.log[0], ContainsSubstring("not in the pack"));
    // Asking again does not report it again.
    CHECK(rig.vm.load_class("ghost") == nullptr);
    CHECK(rig.vm.errors() == 1);

    CHECK(rig.vm.load_class("junk") == nullptr);
    CHECK_THAT(rig.log.back(), ContainsSubstring("junk"));
    CHECK(rig.vm.load_class("misfiled") == nullptr);
    CHECK_THAT(rig.log.back(), ContainsSubstring("SomethingElse"));
    CHECK(rig.vm.load_class("future") == nullptr);
    CHECK_THAT(rig.log.back(), ContainsSubstring("2"));

    // A parent that is missing, and a class that is its own ancestor, end the
    // walk instead of looping.
    CHECK(rig.vm.load_class("orphan") == nullptr);
    CHECK(rig.vm.load_class("loop") == nullptr);
    ScriptSpec ping = script("Ping", {});
    ping.parent = "Pong";
    ScriptSpec pong = script("Pong", {});
    pong.parent = "Ping";
    rig.add(ping);
    rig.add(pong);
    CHECK(rig.vm.load_class("ping") == nullptr);
    CHECK(rig.vm.load_class("pong") == nullptr);
}

// ---- script assets ----------------------------------------------------------

namespace {

/// What `ScriptClass::load` says about `spec`'s asset; empty if it loads.
std::string refusal(const ScriptSpec& spec) {
    std::string error;
    const auto cls = skydot::vm::ScriptClass::load(build_script(spec), error);
    return cls == nullptr ? error : std::string();
}

/// A script whose one function is the single instruction `op args`.
ScriptSpec with_instruction(Op op, std::vector<Arg> args) {
    FunctionSpec f{.name = "f", .global = true};
    f.add(op, std::move(args));
    return script("Subject", {std::move(f)});
}

} // namespace

TEST_CASE("a script asset is indexed: names, states, variables, properties", "[vm][script]") {
    auto in_idle = method("Describe", {}, {}, "String", "Idle");
    in_idle.add(Op::return_, {str("idle")});
    ScriptSpec spec = script("Subject", {method("Run"), in_idle}, {{"Count", "Int", integer(3)}});
    spec.parent = "Base";
    spec.auto_state = "Idle";
    spec.properties = {{.name = "Count", .type = "Int", .auto_var = "Count"}};
    std::string error;
    const auto cls = skydot::vm::ScriptClass::load(build_script(spec), error);
    REQUIRE(cls != nullptr);
    CHECK(error.empty());
    CHECK(cls->name() == "Subject");
    CHECK(cls->key() == "subject");
    CHECK(cls->parent_key() == "base");
    CHECK(cls->auto_state() == "idle");
    CHECK_FALSE(cls->engine_type());

    // Functions are looked up by lowercase state and name.
    REQUIRE(cls->find("", "run") != nullptr);
    CHECK(cls->find("", "run")->state.empty());
    REQUIRE(cls->find("idle", "describe") != nullptr);
    CHECK(cls->find("idle", "describe")->state == "idle");
    CHECK(cls->find("idle", "describe")->return_type == "string");
    CHECK(cls->find("", "describe") == nullptr);
    CHECK(cls->find("busy", "run") == nullptr);

    REQUIRE(cls->variables().size() == 1);
    CHECK(cls->variables()[0].name == "count");
    CHECK(cls->variables()[0].type == "int");
    const auto* property = cls->property("count");
    REQUIRE(property != nullptr);
    CHECK(property->type == "int");
    CHECK(property->auto_var == std::optional<std::uint32_t>(0));
    CHECK(cls->property("missing") == nullptr);

    // A class that declares natives is a type the engine provides.
    ScriptSpec natives = script("Provided", {native_function("Go")});
    const auto provided = skydot::vm::ScriptClass::load(build_script(natives), error);
    REQUIRE(provided != nullptr);
    CHECK(provided->engine_type());
}

TEST_CASE("a script asset that is not one, or is of another version, is refused", "[vm][script]") {
    std::string error;
    CHECK(skydot::vm::ScriptClass::load({}, error) == nullptr);
    CHECK(error == "not a valid script asset");
    CHECK(skydot::vm::ScriptClass::load({'B', 'P', 'X', '1', 1, 2, 3}, error) == nullptr);
    CHECK(error == "not a valid script asset");

    auto bytes = build_script(script("Subject", {method("Run")}));
    for (const std::size_t keep : {bytes.size() / 2, bytes.size() - 1}) {
        auto cut = bytes;
        cut.resize(keep);
        CHECK(skydot::vm::ScriptClass::load(std::move(cut), error) == nullptr);
    }

    ScriptSpec other = script("Subject", {});
    other.format_version = 2;
    const auto why = refusal(other);
    CHECK_THAT(why, ContainsSubstring("2"));
    CHECK_THAT(why, ContainsSubstring("reads 1"));
}

// A failing callstatic prints its class and function from the string table,
// so the loader insists both are names: a literal would index the table with
// its value (out of bounds, and script_class.hpp promises that running a class
// never does). Real PEX never does this, but packs are untrusted files.
TEST_CASE("a call that names its class or function by a literal is refused", "[vm][script]") {
    for (const auto& call : {
             with_instruction(Op::callstatic, {integer(1000), ident("Run"), ident("r")}),
             with_instruction(Op::callstatic, {ident("Util"), integer(1000), ident("r")}),
             with_instruction(Op::callstatic, {ident("Util"), floating(1.0F), ident("r")}),
         }) {
        CHECK_THAT(refusal(call), ContainsSubstring("by a literal"));
    }
    // A name given as a string is still a name.
    CHECK(refusal(with_instruction(Op::callstatic, {str("Util"), str("Run"), ident("r")})).empty());
}

TEST_CASE("loading checks what the interpreter will follow", "[vm][script]") {
    // Instructions with the wrong number of arguments.
    CHECK_THAT(refusal(with_instruction(Op::iadd, {ident("a"), integer(1)})), ContainsSubstring("malformed"));
    CHECK_THAT(refusal(with_instruction(Op::nop, {integer(1)})), ContainsSubstring("malformed"));
    CHECK_THAT(refusal(with_instruction(Op::callmethod, {ident("a"), ident("b")})), ContainsSubstring("malformed"));
    // A call takes any number of arguments after its fixed ones.
    CHECK(refusal(with_instruction(Op::callstatic, {ident("A"), ident("B"), ident("C"), integer(1), integer(2)}))
              .empty());

    // A jump out of the function; one to its end is fine.
    CHECK_THAT(refusal(with_instruction(Op::jmp, {integer(5)})), ContainsSubstring("leaves the function"));
    CHECK_THAT(refusal(with_instruction(Op::jmp, {integer(-1)})), ContainsSubstring("leaves the function"));
    CHECK(refusal(with_instruction(Op::jmp, {integer(1)})).empty());

    // A name outside the string table, wherever it is.
    CHECK_THAT(refusal(with_instruction(Op::assign, {raw_ident(999), integer(1)})), ContainsSubstring("string table"));
    CHECK_THAT(refusal(script("Subject", {}, {{"v", "Int", raw_ident(999)}})), ContainsSubstring("string table"));
}
