// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Papyrus VM's scheduler (vm/vm.cpp): threads that run cooperatively in
// the order they were made, latent natives that suspend them until the clock
// passes or `notify` names an event, and update timers. docs/papyrus.md,
// "Execution".
#include "support/vm_rig.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace skydot::testing;
using skydot::vm::Op;
using skydot::vm::Value;

TEST_CASE("a thread that uses its instruction budget yields and goes on next update", "[vm][threads]") {
    // Function Spin() global: forever
    auto spin = global_function("spin");
    spin.label("again").jmp("again");
    // Function Count(int n) global: loops to n
    auto count = global_function("count", {{"n", "Int"}}, {{"i", "Int"}, {"t", "Bool"}}, "Int");
    count.label("loop")
        .add(Op::iadd, {ident("i"), ident("i"), integer(1)})
        .add(Op::cmp_lt, {ident("t"), ident("i"), ident("n")})
        .jmpt(ident("t"), "loop")
        .add(Op::return_, {ident("i")});
    auto other = global_function("other");
    note(other, "ran");

    Rig rig;
    rig.add(script("Budget", {spin, count, other}));

    // 100,000 instructions per update, and a thread that never ends does not
    // keep the others from running.
    CHECK(rig.vm.call_global("budget", "spin", {}) != 0);
    CHECK(rig.vm.call_global("budget", "other", {}) != 0);
    rig.vm.update(0.0);
    CHECK(rig.vm.instructions() > 100000);
    CHECK(rig.vm.instructions() < 100010);
    CHECK(rig.notes == std::vector<std::string>{"ran"});
    CHECK(rig.vm.thread_count() == 1);
    rig.vm.update(0.0);
    CHECK(rig.vm.instructions() > 200000);

    // A smaller budget makes a finite loop take several updates.
    Rig small;
    small.add(script("Budget", {count}));
    small.vm.set_budget(100);
    const auto thread = small.vm.call_global("budget", "count", {Int(500)});
    int updates = 0;
    while (!small.vm.take_result(thread) && updates < 1000) {
        small.vm.update(0.0);
        ++updates;
    }
    CHECK(updates > 10);
    CHECK(updates < 1000);
}

// ---- latent natives, notify, timers -----------------------------------------

namespace {

/// Function Run() global: Note(a), Wait(seconds), Note(b)
FunctionSpec waits(const std::string& name, const std::string& before, double seconds, const std::string& after) {
    auto run = global_function(name);
    note(run, before);
    call_static(run, "Util", "Wait", {floating(static_cast<float>(seconds))});
    note(run, after);
    return run;
}

} // namespace

TEST_CASE("a latent native suspends its thread until the clock passes it", "[vm][threads][latent]") {
    Rig rig;
    rig.add(script("Latent", {waits("run", "before", 2.0, "after")}));
    const auto thread = rig.vm.call_global("latent", "run", {});
    REQUIRE(thread != 0);

    rig.vm.update(0.0);
    CHECK(rig.notes == std::vector<std::string>{"before"});
    CHECK(rig.vm.thread_count() == 1);
    CHECK_FALSE(rig.vm.take_result(thread).has_value());

    rig.vm.update(1.0);
    rig.vm.update(0.5);
    CHECK(rig.notes == std::vector<std::string>{"before"});
    CHECK(rig.vm.time() == 1.5);

    // It wakes when the clock reaches the time, not only when it is past it.
    rig.vm.update(0.5);
    CHECK(rig.notes == std::vector<std::string>{"before", "after"});
    CHECK(rig.vm.thread_count() == 0);
    CHECK(rig.vm.take_result(thread).has_value());
}

TEST_CASE("a wait counts from when the native ran, and the result comes after it", "[vm][threads][latent]") {
    // Function Delayed() global: Wait(1), return 7
    auto delayed = global_function("delayed", {}, {}, "Int");
    call_static(delayed, "Util", "Wait", {floating(1.0F)});
    delayed.add(Op::return_, {integer(7)});
    Rig rig;
    rig.add(script("Later", {delayed}));

    rig.vm.update(10.0); // the clock is already at 10
    const auto thread = rig.vm.call_global("later", "delayed", {});
    rig.vm.update(0.0);
    rig.vm.update(0.75);
    CHECK_FALSE(rig.vm.take_result(thread).has_value());
    rig.vm.update(0.25);
    const auto result = rig.vm.take_result(thread);
    REQUIRE(result.has_value());
    CHECK(result->i() == 7);
    // A result is taken once.
    CHECK_FALSE(rig.vm.take_result(thread).has_value());
}

TEST_CASE("threads run in the order they were made, and Wait(0) lets the others go first", "[vm][threads]") {
    auto first = waits("first", "a1", 0.0, "a2");
    auto second = global_function("second");
    note(second, "b1");
    auto third = global_function("third");
    note(third, "c1");
    Rig rig;
    rig.add(script("Order", {first, second, third}));

    rig.vm.call_global("order", "first", {});
    rig.vm.call_global("order", "second", {});
    rig.vm.call_global("order", "third", {});
    rig.vm.update(0.0);
    CHECK(rig.notes == std::vector<std::string>{"a1", "b1", "c1"});
    rig.vm.update(0.0);
    CHECK(rig.notes == std::vector<std::string>{"a1", "b1", "c1", "a2"});
}

namespace {

/// Function Run() global: Note(before), WaitFor(key, timeout), Note(after)
FunctionSpec waits_for(const std::string& name, const std::string& key, double timeout) {
    auto run = global_function(name);
    note(run, "waiting " + name);
    call_static(run, "Util", "WaitFor", {str(key), floating(static_cast<float>(timeout))});
    note(run, "woke " + name);
    return run;
}

} // namespace

TEST_CASE("notify wakes the threads waiting for a named event, whatever its case", "[vm][threads][notify]") {
    Rig rig;
    rig.add(script("Anim", {waits_for("one", "Anim:1:Done", 10.0), waits_for("two", "anim:1:done", 10.0),
                            waits_for("other", "Anim:2:Done", 10.0)}));
    rig.vm.call_global("anim", "one", {});
    rig.vm.call_global("anim", "two", {});
    rig.vm.call_global("anim", "other", {});
    rig.vm.update(0.0);
    CHECK(rig.notes == std::vector<std::string>{"waiting one", "waiting two", "waiting other"});

    // Another event, and time short of the timeout, wake nobody.
    rig.vm.notify("anim:3:done");
    rig.vm.update(5.0);
    CHECK(rig.notes.size() == 3);

    // One notify wakes every thread waiting for it; the third keeps waiting.
    rig.vm.notify("ANIM:1:DONE");
    rig.vm.update(0.0);
    CHECK(rig.notes == std::vector<std::string>{"waiting one", "waiting two", "waiting other", "woke one", "woke two"});
    CHECK(rig.vm.thread_count() == 1);
}

TEST_CASE("a thread waiting for an event gives up at its timeout", "[vm][threads][notify]") {
    Rig rig;
    rig.add(script("Anim", {waits_for("one", "Anim:1:Done", 10.0)}));
    rig.vm.call_global("anim", "one", {});
    rig.vm.update(0.0);
    rig.vm.update(9.5);
    CHECK(rig.notes == std::vector<std::string>{"waiting one"});
    rig.vm.update(0.5);
    CHECK(rig.notes == std::vector<std::string>{"waiting one", "woke one"});
    CHECK(rig.vm.thread_count() == 0);
}

TEST_CASE("a notify nobody waits for is not remembered", "[vm][threads][notify]") {
    Rig rig;
    rig.add(script("Anim", {waits_for("one", "Anim:1:Done", 10.0)}));
    rig.vm.notify("Anim:1:Done");
    rig.vm.call_global("anim", "one", {});
    rig.vm.update(0.0);
    rig.vm.update(1.0);
    CHECK(rig.notes == std::vector<std::string>{"waiting one"});
    CHECK(rig.vm.thread_count() == 1);
}

TEST_CASE("update timers send OnUpdate to the script that registered", "[vm][timers]") {
    // Ticker and Quiet both handle OnUpdate; only the one that registers gets it.
    auto on_update = method("OnUpdate", {}, {});
    on_update.add(Op::iadd, {ident("ticks"), ident("ticks"), integer(1)});
    call_static(on_update, "Util", "NoteInt", {ident("ticks")});
    auto quiet = method("OnUpdate");
    note(quiet, "quiet");
    Rig rig;
    rig.add(script("Ticker", {on_update}, {{"ticks", "Int"}}));
    rig.add(script("Quiet", {quiet}));
    auto* ticker = rig.vm.attach(0x80, "ticker");
    REQUIRE(ticker != nullptr);
    REQUIRE(rig.vm.attach(0x80, "quiet") != nullptr);

    SECTION("once, after its delay") {
        rig.vm.register_update(ticker, 2.0, 0.0);
        rig.vm.update(1.5);
        CHECK(rig.notes.empty());
        rig.vm.update(0.5);
        CHECK(rig.notes == std::vector<std::string>{"1"});
        rig.vm.update(10.0);
        CHECK(rig.notes.size() == 1);
    }
    SECTION("every interval after the delay, until unregistered") {
        rig.vm.register_update(ticker, 1.0, 2.0);
        rig.vm.update(1.0);
        CHECK(rig.notes == std::vector<std::string>{"1"});
        rig.vm.update(1.0);
        CHECK(rig.notes.size() == 1);
        rig.vm.update(1.0);
        CHECK(rig.notes == std::vector<std::string>{"1", "2"});
        rig.vm.unregister_update(ticker);
        rig.vm.update(10.0);
        CHECK(rig.notes.size() == 2);
    }
    SECTION("registering again replaces the timer") {
        rig.vm.register_update(ticker, 1.0, 0.0);
        rig.vm.register_update(ticker, 3.0, 0.0);
        rig.vm.update(2.0);
        CHECK(rig.notes.empty());
        rig.vm.update(1.0);
        CHECK(rig.notes == std::vector<std::string>{"1"});
    }
    CHECK(rig.vm.errors() == 0);
}
