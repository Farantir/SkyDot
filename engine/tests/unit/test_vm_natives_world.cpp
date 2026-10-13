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
    std::uint32_t parent_cell(std::uint32_t ref) const override { return ref == 1 ? 0x100 : 0; }
    bool cell_interior(std::uint32_t cell) const override { return cell == 0x100; }
    bool cell_attached(std::uint32_t cell) const override { return cell == 0x100; }
    bool has_keyword(std::uint32_t form, std::uint32_t keyword) const override {
        return (form == 1 || form == 0x700) && keyword == 0x500; // reference 1 and its base 0x700
    }
    std::uint32_t find_keyword(const std::string& editor_id) const override {
        return editor_id == "TestKeyword" ? 0x500 : 0;
    }
    bool form_list(std::uint32_t list, std::vector<std::uint32_t>& forms) const override {
        if (list != 0x600) {
            return false;
        }
        forms = {0x10, 0x20};
        return true;
    }
    bool message(std::uint32_t, skydot::vm::MessageText&) const override { return false; }
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

TEST_CASE("GetParentCell gives the cell form, which knows whether it is an interior and attached", "[vm][natives]") {
    const FakeWorld world;
    Rig rig;
    rig.add({.name = "Cell",
             .functions = {native_function("IsInterior", {}, "Bool", false),
                           native_function("IsAttached", {}, "Bool", false)}});
    rig.add({.name = "ObjectReference",
             .functions = {native_function("GetParentCell", {}, "Cell", false)}});
    skydot::vm::bind_world_natives(rig.vm, world);

    auto interior = global_function("interior", {{"o", "ObjectReference"}},
                                    {{"c", "Cell"}, {"r", "Bool"}}, "Bool");
    interior.add(Op::callmethod, {ident("GetParentCell"), ident("o"), ident("c")})
        .add(Op::callmethod, {ident("IsInterior"), ident("c"), ident("r")})
        .add(Op::return_, {ident("r")});
    auto attached = global_function("attached", {{"o", "ObjectReference"}},
                                    {{"c", "Cell"}, {"r", "Bool"}}, "Bool");
    attached.add(Op::callmethod, {ident("GetParentCell"), ident("o"), ident("c")})
        .add(Op::callmethod, {ident("IsAttached"), ident("c"), ident("r")})
        .add(Op::return_, {ident("r")});
    rig.add(script("Caller", {interior, attached}));

    const auto placed = rig.vm.object(1, "ObjectReference");
    CHECK(rig.call("caller", "interior", {placed}).b());
    CHECK(rig.call("caller", "attached", {placed}).b());
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("message placeholders take the arguments in order", "[vm][natives]") {
    using skydot::vm::format_message;
    CHECK(format_message("Pulled %.0f of %.0f", {2.0, 3.0}) == "Pulled 2 of 3");
    CHECK(format_message("%.1f%% done", {42.25}) == "42.2% done");
    CHECK(format_message("%d items", {7.0}) == "7 items");
    CHECK(format_message("%.0f and %.0f", {1.0}) == "1 and 0"); // missing arguments are 0
    CHECK(format_message("100% sure, %s", {}) == "100% sure, %s"); // not a placeholder
    CHECK(format_message("no placeholders", {5.0}) == "no placeholders");
}

TEST_CASE("keywords and form lists are bound by the world natives", "[vm][natives][formlist]") {
    const FakeWorld world;
    Rig rig;
    rig.add({.name = "Form",
             .functions = {native_function("HasKeyword", {{"k", "Keyword"}}, "Bool", false),
                           native_function("GetFormID", {}, "Int", false)}});
    rig.add({.name = "Keyword", .functions = {native_function("GetKeyword", {{"id", "String"}}, "Keyword", true)}});
    rig.add({.name = "FormList",
             .functions = {native_function("GetSize", {}, "Int", false),
                           native_function("GetAt", {{"i", "Int"}}, "Form", false),
                           native_function("Find", {{"f", "Form"}}, "Int", false),
                           native_function("HasForm", {{"f", "Form"}}, "Bool", false),
                           native_function("AddForm", {{"f", "Form"}}, "None", false),
                           native_function("RemoveAddedForm", {{"f", "Form"}}, "None", false),
                           native_function("Revert", {}, "None", false)}});
    skydot::vm::bind_world_natives(rig.vm, world);
    rig.vm.bind("Form", "GetFormID", [](skydot::vm::NativeCall& c) {
        return skydot::vm::NativeResult{.value = Int(static_cast<std::int32_t>(c.self.form()))};
    });

    auto keyed = global_function("keyed", {{"f", "Form"}, {"name", "String"}}, {{"k", "Keyword"}, {"r", "Bool"}}, "Bool");
    keyed.add(Op::callstatic, {ident("Keyword"), ident("GetKeyword"), ident("k"), ident("name")})
        .add(Op::callmethod, {ident("HasKeyword"), ident("f"), ident("r"), ident("k")})
        .add(Op::return_, {ident("r")});
    auto size = global_function("size", {{"l", "FormList"}}, {{"r", "Int"}}, "Int");
    size.add(Op::callmethod, {ident("GetSize"), ident("l"), ident("r")}).add(Op::return_, {ident("r")});
    auto at = global_function("at", {{"l", "FormList"}, {"i", "Int"}}, {{"f", "Form"}, {"r", "Int"}}, "Int");
    at.add(Op::callmethod, {ident("GetAt"), ident("l"), ident("f"), ident("i")})
        .add(Op::callmethod, {ident("GetFormID"), ident("f"), ident("r")})
        .add(Op::return_, {ident("r")});
    auto find = global_function("find", {{"l", "FormList"}, {"f", "Form"}}, {{"r", "Int"}}, "Int");
    find.add(Op::callmethod, {ident("Find"), ident("l"), ident("r"), ident("f")}).add(Op::return_, {ident("r")});
    auto has = global_function("has", {{"l", "FormList"}, {"f", "Form"}}, {{"r", "Bool"}}, "Bool");
    has.add(Op::callmethod, {ident("HasForm"), ident("l"), ident("r"), ident("f")}).add(Op::return_, {ident("r")});
    auto add = global_function("add", {{"l", "FormList"}, {"f", "Form"}}, {}, "None");
    add.add(Op::callmethod, {ident("AddForm"), ident("l"), ident("::NoneVar"), ident("f")}).add(Op::return_, {none()});
    auto remove = global_function("remove", {{"l", "FormList"}, {"f", "Form"}}, {}, "None");
    remove.add(Op::callmethod, {ident("RemoveAddedForm"), ident("l"), ident("::NoneVar"), ident("f")}).add(Op::return_, {none()});
    auto revert = global_function("revert", {{"l", "FormList"}}, {}, "None");
    revert.add(Op::callmethod, {ident("Revert"), ident("l"), ident("::NoneVar")}).add(Op::return_, {none()});
    rig.add(script("Caller", {keyed, size, at, find, has, add, remove, revert}));

    SECTION("a reference has its base's keywords") {
        const bool k = rig.call("caller", "keyed", {rig.vm.object(1, "ObjectReference"), String("TestKeyword")}).b();
        CAPTURE(rig.log);
        CHECK(k);
        CHECK(rig.call("caller", "keyed", {rig.vm.object(0x700, "Form"), String("TestKeyword")}).b());
        CHECK_FALSE(rig.call("caller", "keyed", {rig.vm.object(2, "Form"), String("TestKeyword")}).b());
    }
    SECTION("a list holds the world's forms, then those added") {
        const auto list = rig.vm.object(0x600, "FormList");
        const auto three = rig.vm.object(0x30, "Form");
        CHECK(rig.call("caller", "size", {list}).i() == 2);
        CHECK(rig.call("caller", "at", {list, Int(1)}).i() == 0x20);
        CHECK(rig.call("caller", "find", {list, rig.vm.object(0x20, "Form")}).i() == 1);
        CHECK(rig.call("caller", "find", {list, three}).i() == -1);
        CHECK_FALSE(rig.call("caller", "has", {list, three}).b());

        rig.call("caller", "add", {list, three});
        rig.call("caller", "add", {list, three});          // once
        rig.call("caller", "add", {list, rig.vm.object(0x10, "Form")}); // already there
        CHECK(rig.call("caller", "size", {list}).i() == 3);
        CHECK(rig.call("caller", "at", {list, Int(2)}).i() == 0x30);
        CHECK(rig.call("caller", "has", {list, three}).b());

        rig.call("caller", "remove", {list, rig.vm.object(0x10, "Form")}); // the world's stays
        rig.call("caller", "remove", {list, three});
        CHECK(rig.call("caller", "size", {list}).i() == 2);
        rig.call("caller", "add", {list, three});
        rig.call("caller", "revert", {list});
        CHECK(rig.call("caller", "size", {list}).i() == 2);
        CAPTURE(rig.log);
        CHECK(rig.vm.errors() == 0);
    }
}
