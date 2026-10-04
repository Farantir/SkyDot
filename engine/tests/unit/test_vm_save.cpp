// SPDX-License-Identifier: GPL-3.0-or-later
//
// VM saves (vm/save.cpp) and the VM's random numbers. A save is the whole of
// what scripts need to carry on: instances with their variables and states,
// threads with every frame, timers, results, the clock and the random state.
// docs/papyrus.md ("Saving"): loading treats the bytes as untrusted, and a save
// whose frames do not match the scripts is refused.
#include "support/vm_rig.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace skydot::testing;
using skydot::vm::Kind;
using skydot::vm::Op;
using skydot::vm::Value;
using Catch::Matchers::ContainsSubstring;

namespace {

/// Saver: a thread that sits inside a nested call. Run() makes the array
/// `list`, aliases it, points `me` at self, then calls Step(1), which waits
/// five seconds and returns 101; Run goes on to store the result, write into
/// the aliased array, note "done" and return the total. `with_step` and
/// `extra_local` make the script differ from the one a save was made with.
ScriptSpec saver(bool with_step = true, bool extra_local = false) {
    auto step = method("Step", {{"x", "Int"}}, {{"r", "Int"}}, "Int");
    if (extra_local) {
        step.locals.push_back({"unexpected", "Int"});
    }
    call_static(step, "Util", "Wait", {floating(5.0F)});
    step.add(Op::iadd, {ident("r"), ident("x"), integer(100)}).add(Op::return_, {ident("r")});

    auto run = method("Run", {}, {{"alias", "Int[]"}, {"got", "Int"}}, "Int");
    run.add(Op::array_create, {ident("list"), integer(3)})
        .add(Op::array_setelement, {ident("list"), integer(0), integer(10)})
        .add(Op::assign, {ident("alias"), ident("list")})
        .add(Op::assign, {ident("me"), ident("self")})
        .add(Op::assign, {ident("label"), str("running")})
        .add(Op::callmethod, {ident("Step"), ident("self"), ident("got"), integer(1)})
        .add(Op::assign, {ident("total"), ident("got")})
        .add(Op::array_setelement, {ident("alias"), integer(2), integer(7)});
    note(run, "done");
    run.add(Op::return_, {ident("total")});

    auto on_update = method("OnUpdate");
    note(on_update, "tick");

    std::vector<FunctionSpec> functions{run, on_update};
    if (with_step) {
        functions.push_back(step);
    }
    return script("Saver", functions,
                  {{"total", "Int"},
                   {"list", "Int[]"},
                   {"me", "Saver"},
                   {"label", "String", str("start")},
                   {"ratio", "Float", floating(0.1F)},
                   {"armed", "Bool", boolean(true)}});
}

/// A rig with Saver and a Saver attached to form 0x10.
struct SaverRig {
    Rig rig;
    skydot::vm::Instance* instance = nullptr;

    explicit SaverRig(const ScriptSpec& spec = saver(), bool attach = true) {
        rig.add(spec);
        if (attach) {
            instance = rig.vm.attach(0x10, "saver");
        }
    }
    skydot::vm::Vm& vm() { return rig.vm; }
    [[nodiscard]] Value variable(const char* name) const { return rig.vm.get_variable(instance, name); }
};

} // namespace

TEST_CASE("a save carries a suspended thread into a fresh VM, which finishes it as the first would", "[vm][save]") {
    SaverRig a;
    REQUIRE(a.instance != nullptr);
    const auto thread = a.vm().call_method(a.instance, "run", {});
    REQUIRE(thread != 0);
    a.vm().update(1.0); // Run is inside Step now, waiting until 6.0
    REQUIRE(a.vm().thread_count() == 1);
    CHECK(a.variable("label").s == "running");
    const auto bytes = a.vm().save();

    SaverRig b(saver(), false);
    std::string error;
    REQUIRE(b.vm().load(bytes, error));
    CHECK(error.empty());
    b.instance = b.vm().instance(0x10, "saver");
    REQUIRE(b.instance != nullptr);

    // What the first VM had when it was saved.
    CHECK(b.vm().time() == 1.0);
    CHECK(b.vm().thread_count() == 1);
    CHECK(b.variable("label").s == "running");
    CHECK(b.variable("total").i == 0);
    CHECK(b.variable("ratio").f == 0.1F);
    CHECK(b.variable("armed").b);
    REQUIRE(b.variable("list").kind == Kind::array);
    CHECK(b.variable("list").array->size() == 3);
    CHECK(b.variable("list").array->at(0).i == 10);

    // The object variable names the loaded instance, not the saved one.
    const auto me = b.variable("me");
    CHECK(me.kind == Kind::object);
    CHECK(me.form == 0x10);
    CHECK(me.instance == b.instance);
    CHECK(me.cls == b.vm().load_class("saver"));

    // Still waiting just short of the wake-up time, in both.
    for (auto* r : {&a, &b}) {
        r->vm().update(4.5);
        CHECK(r->rig.notes.empty());
    }
    // Then both finish alike: the result of Step reaches Run, the aliased
    // array (which the variable shares) is written, and the result is kept.
    for (auto* r : {&a, &b}) {
        r->vm().update(0.5);
        CHECK(r->rig.notes == std::vector<std::string>{"done"});
        CHECK(r->vm().thread_count() == 0);
        CHECK(r->variable("total").i == 101);
        CHECK(r->variable("list").array->at(0).i == 10);
        CHECK(r->variable("list").array->at(1).i == 0);
        CHECK(r->variable("list").array->at(2).i == 7);
        const auto result = r->vm().take_result(thread);
        REQUIRE(result.has_value());
        CHECK(result->i == 101);
        CHECK(r->vm().errors() == 0);
    }
}

TEST_CASE("two VMs that did the same save the same bytes", "[vm][save]") {
    const auto run_and_save = [] {
        SaverRig s;
        s.vm().call_method(s.instance, "run", {});
        s.vm().update(1.0);
        s.vm().register_update(s.instance, 3.0, 2.0);
        s.vm().random();
        return s.vm().save();
    };
    CHECK(run_and_save() == run_and_save());

    // And a loaded VM saves what it was loaded from.
    SaverRig a;
    a.vm().call_method(a.instance, "run", {});
    a.vm().update(1.0);
    a.vm().register_update(a.instance, 3.0, 2.0);
    const auto bytes = a.vm().save();
    SaverRig b(saver(), false);
    std::string error;
    REQUIRE(b.vm().load(bytes, error));
    CHECK(b.vm().save() == bytes);
}

TEST_CASE("a save carries states, timers, results, the clock and thread numbering", "[vm][save]") {
    SaverRig a;
    a.instance->state = "angry";
    a.vm().update(2.0);
    a.vm().register_update(a.instance, 1.0, 2.0); // due at 3, 5, 7...
    // A call that has finished, its result not yet taken.
    const auto finished = a.vm().call_method(a.instance, "run", {});
    a.vm().update(10.0); // clock 12; Run is waiting for Step until 17
    const auto waiting = a.vm().call_method(a.instance, "run", {});
    a.vm().update(0.0);
    const auto bytes = a.vm().save();

    SaverRig b(saver(), false);
    std::string error;
    REQUIRE(b.vm().load(bytes, error));
    b.instance = b.vm().instance(0x10, "saver");
    REQUIRE(b.instance != nullptr);
    CHECK(b.instance->state == "angry");
    CHECK(b.vm().time() == 12.0);

    // The timer is the one registered: OnUpdate is not due again before 13...
    b.rig.notes.clear();
    b.vm().update(0.5);
    CHECK(b.rig.notes.empty());
    // ...but is at the next interval it had.
    b.vm().update(0.5);
    CHECK(b.rig.notes == std::vector<std::string>{"tick"});

    // Threads made after loading do not reuse a number.
    const auto fresh = b.vm().call_method(b.instance, "run", {});
    CHECK(fresh != finished);
    CHECK(fresh != waiting);
    CHECK(fresh > std::max(finished, waiting));
}

TEST_CASE("a finished call's result survives a save until it is taken", "[vm][save]") {
    // Function Answer() global: 42
    auto answer = global_function("answer", {}, {}, "Int");
    answer.add(Op::return_, {integer(42)});
    Rig a;
    a.add(script("Answer", {answer}));
    const auto thread = a.vm.call_global("answer", "answer", {});
    a.vm.update(0.0);
    const auto bytes = a.vm.save();

    Rig b;
    b.add(script("Answer", {answer}));
    std::string error;
    REQUIRE(b.vm.load(bytes, error));
    const auto result = b.vm.take_result(thread);
    REQUIRE(result.has_value());
    CHECK(result->i == 42);
}

TEST_CASE("random numbers repeat from one VM to another, and from a save", "[vm][random]") {
    skydot::vm::Vm a;
    skydot::vm::Vm b;
    std::vector<std::uint32_t> first;
    for (int i = 0; i < 100; ++i) {
        first.push_back(a.random());
        CHECK(first.back() == b.random());
    }
    // They are not one number over and over.
    const std::set<std::uint32_t> distinct(first.begin(), first.end());
    CHECK(distinct.size() > 90);
    // A third that is only started gives the same first numbers.
    skydot::vm::Vm c;
    CHECK(c.random() == first[0]);
    CHECK(c.random() == first[1]);

    // The state is part of the save: a VM loaded from one carries on where the
    // saved one was.
    for (int i = 0; i < 7; ++i) {
        a.random();
    }
    const auto bytes = a.save();
    skydot::vm::Vm loaded;
    std::string error;
    REQUIRE(loaded.load(bytes, error));
    for (int i = 0; i < 20; ++i) {
        CHECK(loaded.random() == a.random());
    }
}

TEST_CASE("loading replaces everything the VM had", "[vm][save]") {
    SaverRig empty(saver(), false);
    const auto nothing = empty.vm().save();

    SaverRig busy;
    busy.vm().call_method(busy.instance, "run", {});
    busy.vm().update(1.0);
    REQUIRE(busy.vm().thread_count() == 1);
    REQUIRE(busy.vm().instance(0x10, "saver") != nullptr);

    std::string error;
    REQUIRE(busy.vm().load(nothing, error));
    CHECK(busy.vm().instances(0x10).empty());
    CHECK(busy.vm().thread_count() == 0);
    CHECK(busy.vm().time() == 0.0);
}

// ---- untrusted bytes --------------------------------------------------------

namespace {

/// A save with an instance, a thread two frames deep, a timer and a result.
std::vector<std::uint8_t> sample_save() {
    SaverRig s;
    s.vm().call_method(s.instance, "run", {});
    s.vm().update(1.0);
    s.vm().register_update(s.instance, 3.0, 0.0);
    return s.vm().save();
}

void check_refused_and_empty(skydot::vm::Vm& vm, const std::vector<std::uint8_t>& bytes) {
    std::string error;
    CHECK_FALSE(vm.load(bytes, error));
    CHECK_FALSE(error.empty());
    // On failure the VM is left empty.
    CHECK(vm.instances(0x10).empty());
    CHECK(vm.thread_count() == 0);
}

} // namespace

TEST_CASE("bytes that are not a VM save are refused and leave the VM empty", "[vm][save][errors]") {
    SaverRig s(saver(), false);
    auto bytes = sample_save();

    SECTION("nothing") { check_refused_and_empty(s.vm(), {}); }
    SECTION("another file") {
        check_refused_and_empty(s.vm(), {'N', 'O', 'P', 'E', 1, 0, 0, 0});
    }
    SECTION("a version this engine does not read") {
        bytes[4] = 2;
        std::string error;
        CHECK_FALSE(s.vm().load(bytes, error));
        CHECK_THAT(error, ContainsSubstring("version 2"));
    }
    SECTION("bytes after the end") {
        bytes.push_back(0);
        std::string error;
        CHECK_FALSE(s.vm().load(bytes, error));
        CHECK_THAT(error, ContainsSubstring("after the end"));
        CHECK(s.vm().instances(0x10).empty());
    }
}

TEST_CASE("every truncation of a save is refused", "[vm][save][errors]") {
    const auto bytes = sample_save();
    SaverRig s(saver(), false);
    for (std::size_t keep = 0; keep < bytes.size(); ++keep) {
        INFO("kept " << keep << " of " << bytes.size() << " bytes");
        const std::vector<std::uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(keep));
        check_refused_and_empty(s.vm(), cut);
    }
}

TEST_CASE("a damaged save is refused or loads, never crashes", "[vm][save][errors]") {
    const auto bytes = sample_save();
    SaverRig s(saver(), false);
    for (const std::uint8_t damage : {std::uint8_t{0xFF}, std::uint8_t{0x00}, std::uint8_t{0x7F}}) {
        for (std::size_t at = 0; at < bytes.size(); ++at) {
            auto bad = bytes;
            bad[at] = damage;
            std::string error;
            if (!s.vm().load(bad, error)) {
                CHECK_FALSE(error.empty());
                CHECK(s.vm().instances(0x10).empty());
                CHECK(s.vm().thread_count() == 0);
            }
            // What loaded must run without crashing.
            s.vm().update(10.0);
        }
    }
}

TEST_CASE("a save of other scripts than the pack's is refused", "[vm][save][errors]") {
    const auto bytes = sample_save();
    std::string error;

    SECTION("a function the script no longer has") {
        SaverRig changed(saver(false), false);
        CHECK_FALSE(changed.vm().load(bytes, error));
        CHECK_THAT(error, ContainsSubstring("step"));
        CHECK(changed.vm().instances(0x10).empty());
        CHECK(changed.vm().thread_count() == 0);
    }
    SECTION("a frame whose registers no longer fit") {
        SaverRig changed(saver(true, true), false);
        CHECK_FALSE(changed.vm().load(bytes, error));
        CHECK_THAT(error, ContainsSubstring("does not match"));
        CHECK(changed.vm().thread_count() == 0);
    }
    SECTION("a script the pack lacks") {
        Rig changed;
        CHECK_FALSE(changed.vm.load(bytes, error));
        CHECK_THAT(error, ContainsSubstring("saver"));
        CHECK(changed.vm.instances(0x10).empty());
    }
}
