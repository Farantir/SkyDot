// SPDX-License-Identifier: GPL-3.0-or-later
//
// Utility natives (vm/natives_utility.cpp): the wait, random numbers and the
// VM's run time.
#include "support/vm_rig.hpp"

#include "vm/natives_families.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace skydot::testing;
using skydot::vm::Op;

namespace {

void add_utility(Rig& rig) {
    rig.add({.name = "Utility",
             .functions = {native_function("RandomInt", {{"lo", "Int"}, {"hi", "Int"}}, "Int"),
                           native_function("RandomFloat", {{"lo", "Float"}, {"hi", "Float"}}, "Float"),
                           native_function("GetCurrentRealTime", {}, "Float"),
                           native_function("Wait", {{"seconds", "Float"}}, "None")}});
    skydot::vm::bind_utility_natives(rig.vm);
}

} // namespace

TEST_CASE("RandomInt stays within its inclusive bounds and swaps reversed ones", "[vm][natives]") {
    Rig rig;
    add_utility(rig);
    auto f = global_function("roll", {{"lo", "Int"}, {"hi", "Int"}}, {{"r", "Int"}}, "Int");
    f.add(Op::callstatic, {ident("Utility"), ident("RandomInt"), ident("r"), ident("lo"), ident("hi")})
        .add(Op::return_, {ident("r")});
    rig.add(script("Caller", {f}));

    bool low = false;
    bool high = false;
    for (int i = 0; i < 200; ++i) {
        const auto n = rig.call("caller", "roll", {Int(3), Int(5)}).i();
        REQUIRE(n >= 3);
        REQUIRE(n <= 5);
        low = low || n == 3;
        high = high || n == 5;
        const auto swapped = rig.call("caller", "roll", {Int(5), Int(3)}).i();
        REQUIRE(swapped >= 3);
        REQUIRE(swapped <= 5);
    }
    CHECK(low);
    CHECK(high);
    CHECK(rig.call("caller", "roll", {Int(7), Int(7)}).i() == 7);
}

TEST_CASE("RandomFloat is between its bounds", "[vm][natives]") {
    Rig rig;
    add_utility(rig);
    auto f = global_function("roll", {{"lo", "Float"}, {"hi", "Float"}}, {{"r", "Float"}}, "Float");
    f.add(Op::callstatic, {ident("Utility"), ident("RandomFloat"), ident("r"), ident("lo"), ident("hi")})
        .add(Op::return_, {ident("r")});
    rig.add(script("Caller", {f}));

    for (int i = 0; i < 100; ++i) {
        const float x = rig.call("caller", "roll", {Float(2.0F), Float(4.0F)}).f();
        REQUIRE(x >= 2.0F);
        REQUIRE(x <= 4.0F);
    }
    CHECK(rig.call("caller", "roll", {Float(1.5F), Float(1.5F)}).f() == 1.5F);
}

TEST_CASE("Wait waits the seconds given, never less than none, and GetCurrentRealTime is the VM clock",
          "[vm][natives]") {
    Rig rig;
    add_utility(rig);
    auto f = global_function("pause", {{"s", "Float"}}, {{"t", "Float"}}, "Float");
    call_static(f, "Utility", "Wait", {ident("s")});
    f.add(Op::callstatic, {ident("Utility"), ident("GetCurrentRealTime"), ident("t")}).add(Op::return_, {ident("t")});
    rig.add(script("Caller", {f}));

    const auto thread = rig.vm.call_global("caller", "pause", {Float(2.0F)});
    rig.vm.run_ready();
    CHECK_FALSE(rig.vm.take_result(thread).has_value()); // still waiting
    rig.vm.update(1.0);
    CHECK_FALSE(rig.vm.take_result(thread).has_value());
    rig.vm.update(1.5);
    const auto done = rig.vm.take_result(thread);
    REQUIRE(done.has_value());
    CHECK(done->f() >= 2.0F);

    // A negative wait is no wait: the next update finishes it, with no time passed.
    const auto negative = rig.vm.call_global("caller", "pause", {Float(-3.0F)});
    rig.vm.run_ready();
    rig.vm.update(0.0);
    rig.vm.update(0.0);
    CHECK(rig.vm.take_result(negative).has_value());
}
