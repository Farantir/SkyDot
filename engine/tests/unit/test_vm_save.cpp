// SPDX-License-Identifier: GPL-3.0-or-later
//
// VM saves (vm/save.cpp) and the VM's random numbers. A save is the whole of
// what scripts need to carry on: instances with their variables and states,
// threads with every frame, timers, results, the clock and the random state.
// docs/papyrus.md ("Saving"): loading treats the bytes as untrusted, and a save
// whose frames do not match the scripts is refused.
#include "support/vm_rig.hpp"
#include "vm/inventory.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
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
    CHECK(a.variable("label").s() == "running");
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
    CHECK(b.variable("label").s() == "running");
    CHECK(b.variable("total").i() == 0);
    CHECK(b.variable("ratio").f() == 0.1F);
    CHECK(b.variable("armed").b());
    REQUIRE(b.variable("list").kind() == Kind::array);
    CHECK(b.variable("list").array()->size() == 3);
    CHECK(b.variable("list").array()->at(0).i() == 10);

    // The object variable names the loaded instance, not the saved one.
    const auto me = b.variable("me");
    CHECK(me.kind() == Kind::object);
    CHECK(me.form() == 0x10);
    CHECK(me.instance() == b.instance);
    CHECK(me.cls() == b.vm().load_class("saver"));

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
        CHECK(r->variable("total").i() == 101);
        CHECK(r->variable("list").array()->at(0).i() == 10);
        CHECK(r->variable("list").array()->at(1).i() == 0);
        CHECK(r->variable("list").array()->at(2).i() == 7);
        const auto result = r->vm().take_result(thread);
        REQUIRE(result.has_value());
        CHECK(result->i() == 101);
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
    a.instance->set_state("angry");
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
    CHECK(b.instance->state() == "angry");
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
    CHECK(result->i() == 42);
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
        bytes[4] = 7;
        std::string error;
        CHECK_FALSE(s.vm().load(bytes, error));
        CHECK_THAT(error, ContainsSubstring("version 7"));
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

namespace {

using Bytes = std::vector<std::uint8_t>;

void put_u32(Bytes& out, std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) {
        out.push_back(static_cast<std::uint8_t>((v >> s) & 0xFFU));
    }
}

/// A hand-written save: one instance of `Holder` (form 0x10) whose only
/// variable is `list`, and no threads, results or timers. `value` is the
/// bytes of that variable's value.
Bytes holder_save(const Bytes& value) {
    Bytes out;
    const auto text = [&](std::string_view t) {
        put_u32(out, static_cast<std::uint32_t>(t.size()));
        out.insert(out.end(), t.begin(), t.end());
    };
    out.insert(out.end(), {'S', 'K', 'P', 'V'});
    put_u32(out, 1);                     // version
    out.insert(out.end(), 8 + 8 + 8, 0); // time, next thread, random state
    put_u32(out, 1);                     // instances
    put_u32(out, 0x10);
    text("holder");
    text("");
    put_u32(out, 1); // variable levels
    text("holder");
    put_u32(out, 1); // variables
    out.insert(out.end(), value.begin(), value.end());
    put_u32(out, 0); // threads
    put_u32(out, 0); // results
    put_u32(out, 0); // timers
    return out;
}

/// An array value (kind 6, its id, its element count) followed by the
/// elements' bytes. An id seen before is written alone, as `array_ref`.
Bytes array_of(std::uint32_t id, std::uint32_t count, const Bytes& elements) {
    Bytes out{6};
    put_u32(out, id);
    put_u32(out, count);
    out.insert(out.end(), elements.begin(), elements.end());
    return out;
}

Bytes array_ref(std::uint32_t id) {
    Bytes out{6};
    put_u32(out, id);
    return out;
}

Bytes int_value(std::uint32_t v) {
    Bytes out{1};
    put_u32(out, v);
    return out;
}

} // namespace

// An array that holds itself, or two that hold each other, made
// Vm::to_string recurse until the stack ran out. Papyrus has no arrays of
// arrays, so a save with one is not from the game and is refused.
TEST_CASE("a save whose arrays hold arrays is refused", "[vm][save][errors]") {
    SaverRig s(script("Holder", {}, {{"list", "Int[]"}}), false);

    SECTION("an array holding itself") { check_refused_and_empty(s.vm(), holder_save(array_of(0, 1, array_ref(0)))); }
    SECTION("two arrays holding each other") {
        check_refused_and_empty(s.vm(), holder_save(array_of(0, 1, array_of(1, 1, array_ref(0)))));
    }
    SECTION("an array holding another, with no cycle") {
        check_refused_and_empty(s.vm(), holder_save(array_of(0, 1, array_of(1, 1, int_value(5)))));
    }
    SECTION("the same layout with plain elements loads") {
        Bytes elements = int_value(5);
        const Bytes second = int_value(6);
        elements.insert(elements.end(), second.begin(), second.end());
        std::string error;
        REQUIRE(s.vm().load(holder_save(array_of(0, 2, elements)), error));
        auto* holder = s.vm().instance(0x10, "holder");
        REQUIRE(holder != nullptr);
        const auto list = s.vm().get_variable(holder, "list");
        REQUIRE(list.kind() == Kind::array);
        REQUIRE(list.array()->size() == 2);
        CHECK(list.array()->at(1).i() == 6);
    }
}

TEST_CASE("host calls scheduled on the VM's clock are saved and run once due", "[vm][save]") {
    SaverRig a(saver(), false);
    std::vector<std::string> ran;
    const auto record = [&ran](const std::string& kind, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        ran.push_back(kind + ":" + std::to_string(x) + ":" + std::to_string(y) + ":" + std::to_string(z));
    };
    a.vm().set_scheduled_handler(record);
    a.vm().schedule(2.0, "activate", 7, 20, 2);
    a.vm().schedule(0.0, "activate", 8, 20, 1);
    a.vm().update(0.5);
    REQUIRE(ran == std::vector<std::string>{"activate:8:20:1"});
    REQUIRE(a.vm().scheduled_count() == 1);
    const auto bytes = a.vm().save();

    SaverRig b(saver(), false);
    ran.clear();
    b.vm().set_scheduled_handler(record);
    std::string error;
    REQUIRE(b.vm().load(bytes, error));
    CHECK(b.vm().scheduled_count() == 1);
    b.vm().update(1.0);
    CHECK(ran.empty());
    b.vm().update(1.0);
    CHECK(ran == std::vector<std::string>{"activate:7:20:2"});
    CHECK(b.vm().scheduled_count() == 0);
}

TEST_CASE("inventories add, remove and count per owner", "[vm][inventory]") {
    skydot::vm::Inventory inv;
    CHECK(inv.count(0x14, 0x39647) == 0);
    inv.add(0x14, 0x39647, 2);
    inv.add(0x14, 0x39647, 1);
    inv.add(0x14, 0x1, 0); // nothing to add
    CHECK(inv.count(0x14, 0x39647) == 3);
    CHECK(inv.count(0x15, 0x39647) == 0);
    CHECK(inv.remove(0x14, 0x39647, 2) == 2);
    CHECK(inv.remove(0x14, 0x39647, 5) == 1); // clamped at what was there
    CHECK(inv.count(0x14, 0x39647) == 0);
    CHECK(inv.remove(0x14, 0x39647, 1) == 0);
    CHECK(inv.owners().empty());
}

TEST_CASE("an inventory is part of the VM save, and a version 2 save loads with none", "[vm][save][inventory]") {
    skydot::vm::Vm a;
    a.inventory().add(0x14, 0x39647, 3);
    a.inventory().add(0x14, 0x0F, 100);
    a.inventory().add(0x4E2B9, 0x39647, 1);
    const auto bytes = a.save();
    skydot::vm::Vm b;
    b.inventory().add(0x99, 0x1, 1); // replaced by the load
    std::string error;
    REQUIRE(b.load(bytes, error));
    CHECK(b.inventory().count(0x14, 0x39647) == 3);
    CHECK(b.inventory().count(0x14, 0x0F) == 100);
    CHECK(b.inventory().count(0x4E2B9, 0x39647) == 1);
    CHECK(b.inventory().count(0x99, 0x1) == 0);

    SECTION("a cut inventory is refused") {
        auto cut = bytes;
        cut.pop_back();
        skydot::vm::Vm c;
        CHECK_FALSE(c.load(cut, error));
    }
    SECTION("version 2") {
        Bytes v2;
        v2.insert(v2.end(), {'S', 'K', 'P', 'V'});
        put_u32(v2, 2);
        v2.insert(v2.end(), 8 + 8 + 8, 0);
        for (int i = 0; i < 5; ++i) { // instances, threads, results, timers, scheduled
            put_u32(v2, 0);
        }
        skydot::vm::Vm d;
        d.inventory().add(0x14, 0x1, 1);
        REQUIRE(d.load(v2, error));
        CHECK(d.inventory().owners().empty());
    }
}

TEST_CASE("animation states are part of the VM save; version 3 saves load with none", "[vm][save][animation]") {
    skydot::vm::Vm a;
    a.set_animation_state(0x3B9F1, {1, 2, 3, 4});
    a.set_animation_state(0x77, {9});
    const auto bytes = a.save();
    skydot::vm::Vm b;
    b.set_animation_state(0x99, {5}); // replaced by the load
    std::string error;
    REQUIRE(b.load(bytes, error));
    REQUIRE(b.animation_states().size() == 2);
    CHECK(b.animation_states().at(0x3B9F1) == std::vector<std::uint8_t>{1, 2, 3, 4});
    CHECK(b.animation_states().at(0x77) == std::vector<std::uint8_t>{9});

    SECTION("a truncated section is refused") {
        auto cut = bytes;
        cut.pop_back();
        skydot::vm::Vm c;
        CHECK_FALSE(c.load(cut, error));
    }
    SECTION("a length beyond the bytes is refused") {
        auto bad = bytes;
        // Last record: ref(4) length(4) 1 byte; point its length far out.
        bad[bad.size() - 5] = 0xFF;
        bad[bad.size() - 4] = 0xFF;
        skydot::vm::Vm c;
        CHECK_FALSE(c.load(bad, error));
    }
    SECTION("version 3") {
        Bytes v3;
        v3.insert(v3.end(), {'S', 'K', 'P', 'V'});
        put_u32(v3, 3);
        v3.insert(v3.end(), 8 + 8 + 8, 0);
        for (int i = 0; i < 6; ++i) { // instances, threads, results, timers, scheduled, inventory
            put_u32(v3, 0);
        }
        skydot::vm::Vm d;
        d.set_animation_state(0x1, {1});
        REQUIRE(d.load(v3, error));
        CHECK(d.animation_states().empty());
    }
}

TEST_CASE("forms added to lists are part of the VM save; version 4 saves load with none", "[vm][save][formlist]") {
    skydot::vm::Vm a;
    a.form_lists().add(0x600, 0x30);
    a.form_lists().add(0x600, 0x31);
    a.form_lists().add(0x601, 0x32);
    const auto bytes = a.save();
    skydot::vm::Vm b;
    b.form_lists().add(0x99, 0x1); // replaced by the load
    std::string error;
    REQUIRE(b.load(bytes, error));
    CHECK(b.form_lists().added(0x600) == std::vector<std::uint32_t>{0x30, 0x31});
    CHECK(b.form_lists().added(0x601) == std::vector<std::uint32_t>{0x32});
    CHECK(b.form_lists().added(0x99).empty());

    auto cut = bytes;
    cut.pop_back();
    skydot::vm::Vm c;
    CHECK_FALSE(c.load(cut, error));

    // A save without the section (version 4) ends after the animation states.
    skydot::vm::Vm empty;
    const auto plain = empty.save();
    Bytes older(plain.begin(), plain.end() - 16); // drop the empty list, placed and destruction sections
    older[4] = 4;                                // the version, little-endian
    skydot::vm::Vm d;
    d.form_lists().add(0x1, 0x2);
    REQUIRE(d.load(older, error));
    CHECK(d.form_lists().lists().empty());
}

TEST_CASE("placed references and destruction are part of the VM save; version 5 saves load with none", "[vm][save][placed]") {
    skydot::vm::Vm a;
    skydot::vm::PlacedRef rock{.base = 0x1234, .cell = 0x77};
    rock.position[0] = 10.5;
    rock.rotation[2] = 1.25;
    const auto kept = a.placed().add(rock);
    skydot::vm::PlacedRef puff = rock;
    puff.transient = true;
    const auto effect = a.placed().add(puff);
    a.destruction().set(0x9B0EC, {.damage = 10.0, .stage = 1, .destroyed = false});
    const auto bytes = a.save();

    skydot::vm::Vm b;
    b.placed().add(rock); // replaced by the load
    std::string error;
    REQUIRE(b.load(bytes, error));
    REQUIRE(b.placed().refs().size() == 1); // effects are not kept
    const auto* loaded = b.placed().find(kept);
    REQUIRE(loaded != nullptr);
    CHECK(loaded->base == 0x1234);
    CHECK(loaded->cell == 0x77);
    CHECK(loaded->position[0] == 10.5);
    CHECK(loaded->rotation[2] == 1.25);
    CHECK(b.placed().find(effect) == nullptr);
    CHECK(b.placed().add(rock) > effect); // ids are not reused
    CHECK(b.destruction().get(0x9B0EC).stage == 1);
    CHECK(b.destruction().get(0x9B0EC).damage == 10.0);

    skydot::vm::Vm empty;
    auto plain = empty.save();
    Bytes older(plain.begin(), plain.end() - 12); // drop the placed and destruction sections
    older[4] = 5;
    skydot::vm::Vm c;
    c.destruction().set(1, {.damage = 1.0, .stage = 1});
    REQUIRE(c.load(older, error));
    CHECK(c.placed().refs().empty());
    CHECK(c.destruction().states().empty());
}

TEST_CASE("detaching a reference drops its scripts, timers, threads, inventory and animation state", "[vm][detach]") {
    SaverRig a;
    auto* other = a.vm().attach(0x20, "saver");
    REQUIRE(other != nullptr);
    a.vm().call_method(a.instance, "run", {}); // waits inside Step for 5 s
    a.vm().call_method(other, "run", {});
    a.vm().update(1.0);
    a.vm().register_update(a.instance, 1.0, 1.0);
    a.vm().register_update(other, 1.0, 1.0);
    a.vm().inventory().add(0x10, 0x500, 3);
    a.vm().inventory().add(0x20, 0x500, 2);
    a.vm().set_animation_state(0x10, {1, 2, 3});
    a.vm().set_animation_state(0x20, {4});
    a.vm().schedule(2.0, "activate", 0x10, 0x14, 0);
    a.vm().schedule(2.0, "activate", 0x20, 0x14, 0);
    REQUIRE(a.vm().thread_count() == 2);

    a.vm().detach(0x10);
    CHECK(a.vm().instances(0x10).empty());
    CHECK(a.vm().instance(0x10, "saver") == nullptr);
    CHECK(a.vm().inventory().count(0x10, 0x500) == 0);
    CHECK(a.vm().animation_states().count(0x10) == 0);
    CHECK(a.vm().scheduled_count() == 1);
    // What belongs to other references stays.
    CHECK(a.vm().instances(0x20).size() == 1);
    CHECK(a.vm().inventory().count(0x20, 0x500) == 2);
    CHECK(a.vm().animation_states().count(0x20) == 1);

    // Its timer does not fire, its thread does not finish; the other's do.
    a.rig.notes.clear();
    a.vm().update(1.0);
    CHECK(a.rig.notes == std::vector<std::string>{"tick"});
    CHECK(a.vm().thread_count() == 1);
    a.vm().update(10.0);
    CHECK(a.vm().thread_count() == 0);
    CHECK(a.vm().errors() == 0);
    CHECK(std::ranges::count(a.rig.notes, "done") == 1);

    // Detaching what is not there is harmless, and the reference can get scripts again.
    a.vm().detach(0x10);
    a.vm().detach(0x9999);
    CHECK(a.vm().attach(0x10, "saver") != nullptr);
}

TEST_CASE("a script can detach its own reference while it runs", "[vm][detach]") {
    auto go = method("Go", {}, {}, "Int");
    call_static(go, "Util", "Detach", {integer(0x10)});
    note(go, "after");
    go.add(Op::return_, {integer(1)});
    auto spec = saver();
    spec.functions.push_back(go);
    SaverRig a(spec);
    a.vm().call_method(a.instance, "go", {});
    a.vm().update(0.0);
    CHECK(a.vm().instances(0x10).empty());
    CHECK(a.vm().thread_count() == 0);
    CHECK(a.rig.notes.empty()); // the thread stopped where it was
    CHECK(a.vm().errors() == 0);
}
