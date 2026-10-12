// SPDX-License-Identifier: GPL-3.0-or-later
//
// Natives written against WorldAccess (vm/natives_world.cpp): linked-ref
// chains and the encounter level, over a world of a few references.
#include "support/vm_rig.hpp"

#include "vm/natives_world.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>

using namespace skydot::testing;
using skydot::vm::Op;

namespace {

/// 1 -> 2 -> 3 (end); 5 -> 6 -> 5 (a loop).
struct FakeWorld final : skydot::vm::WorldAccess {
    std::map<std::uint32_t, std::uint32_t> links{{1, 2}, {2, 3}, {5, 6}, {6, 5}};
    std::uint32_t base_form(std::uint32_t) const override { return 0; }
    std::uint32_t linked_ref(std::uint32_t ref, std::uint32_t keyword) const override {
        const auto it = links.find(ref);
        return keyword == 0 && it != links.end() ? it->second : 0;
    }
    std::int32_t player_level() const override { return 7; }
};

} // namespace

TEST_CASE("the nth linked ref walks the chain: 0 is the reference, 1 its link, 2 the link's link", "[vm][natives]") {
    const FakeWorld world;
    using skydot::vm::nth_linked_ref;
    CHECK(nth_linked_ref(world, 1, 0) == 1);
    CHECK(nth_linked_ref(world, 1, 1) == 2);
    CHECK(nth_linked_ref(world, 1, 2) == 3);
    CHECK(nth_linked_ref(world, 1, 3) == 0); // the chain ended
    CHECK(nth_linked_ref(world, 1, 50) == 0);
    CHECK(nth_linked_ref(world, 1, -1) == 0);
    CHECK(nth_linked_ref(world, 9, 1) == 0); // not linked
    CHECK(nth_linked_ref(world, 5, 3) == 6);  // a loop is walked as far as asked
}

TEST_CASE("GetNthLinkedRef and CalculateEncounterLevel are bound by the world natives", "[vm][natives]") {
    const FakeWorld world;
    Rig rig;
    rig.add({.name = "ObjectReference",
             .functions = {native_function("GetFormID", {}, "Int", false),
                           native_function("GetNthLinkedRef", {{"n", "Int"}}, "ObjectReference", false),
                           native_function("CalculateEncounterLevel", {{"difficulty", "Int"}}, "Int", false)}});
    skydot::vm::bind_world_natives(rig.vm, world);
    rig.vm.bind("ObjectReference", "GetFormID", [](skydot::vm::NativeCall& c) {
        return skydot::vm::NativeResult{.value = Int(static_cast<std::int32_t>(c.self.form()))};
    });

    // Function Nth(ObjectReference o, Int n) global: o.GetNthLinkedRef(n).GetFormID()
    auto nth = global_function("nth", {{"o", "ObjectReference"}, {"n", "Int"}},
                               {{"l", "ObjectReference"}, {"r", "Int"}}, "Int");
    nth.add(Op::callmethod, {ident("GetNthLinkedRef"), ident("o"), ident("l"), ident("n")})
        .add(Op::callmethod, {ident("GetFormID"), ident("l"), ident("r")})
        .add(Op::return_, {ident("r")});
    // Function Level() global: Self-less call through an object
    auto level = global_function("level", {{"o", "ObjectReference"}}, {{"r", "Int"}}, "Int");
    level.add(Op::callmethod, {ident("CalculateEncounterLevel"), ident("o"), ident("r"), integer(0)})
        .add(Op::return_, {ident("r")});
    rig.add(script("Caller", {nth, level}));

    const auto first = rig.vm.object(1, "ObjectReference");
    CHECK(rig.call("caller", "nth", {first, Int(1)}).i() == 2);
    CHECK(rig.call("caller", "nth", {first, Int(2)}).i() == 3);
    CHECK(rig.call("caller", "level", {first}).i() == 7);
    CHECK(rig.vm.errors() == 0);
    // Past the end of the chain the result is None, and calling on it is the script's error.
    CHECK(rig.call("caller", "nth", {first, Int(3)}).i() == 0);
    CHECK(rig.vm.errors() == 1);
}
