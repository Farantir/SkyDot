// SPDX-License-Identifier: GPL-3.0-or-later
//
// The world map natives (vm/natives_map.cpp) and the map state in the VM save (MMRK).
#include "support/vm_rig.hpp"

#include "vm/natives_families.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

using namespace skydot::testing;
using skydot::vm::Op;

namespace {

constexpr std::uint32_t k_marker = 0x162A4; // a hidden marker in the data (flags 0)
constexpr std::uint32_t k_capital = 0x162CE; // visible from the start (flags 1)
constexpr std::uint32_t k_rock = 0x1234; // no map marker

std::optional<std::uint8_t> data_flags(std::uint32_t ref) {
    if (ref == k_marker) {
        return std::uint8_t{0};
    }
    if (ref == k_capital) {
        return std::uint8_t{1};
    }
    return std::nullopt;
}

void add_classes(Rig& rig) {
    rig.add({.name = "ObjectReference",
             .functions = {native_function("AddToMap", {{"abAllowFastTravel", "Bool"}}, "None", false),
                           native_function("EnableFastTravel", {{"abEnable", "Bool"}}, "None", false),
                           native_function("IsMapMarkerVisible", {}, "Bool", false),
                           native_function("CanFastTravelToMarker", {}, "Bool", false)}});
    rig.add({.name = "Game",
             .functions = {native_function("EnableFastTravel", {{"abEnable", "Bool"}}, "None", true),
                           native_function("IsFastTravelEnabled", {}, "Bool", true),
                           native_function("FastTravel", {{"akDestination", "ObjectReference"}}, "None", true)}});
}

/// A global function `name`(o) that calls method `fn` of o with `args` and returns its Bool (or true).
FunctionSpec method_call(const std::string& name, const std::string& fn, std::vector<Arg> args, bool returns) {
    auto f = global_function(name, {{"o", "ObjectReference"}}, {{"r", "Bool"}}, "Bool");
    std::vector<Arg> call{ident(fn), ident("o"), ident(returns ? "r" : "::NoneVar")};
    call.insert(call.end(), args.begin(), args.end());
    f.add(Op::callmethod, call);
    f.add(Op::return_, {returns ? ident("r") : boolean(true)});
    return f;
}

} // namespace

TEST_CASE("AddToMap shows a marker and, when asked, lets the player travel there", "[vm][natives][map]") {
    Rig rig;
    add_classes(rig);
    skydot::vm::bind_map_natives(rig.vm, data_flags);
    rig.add(script("Caller", {method_call("add", "AddToMap", {boolean(false)}, false),
                              method_call("addtravel", "AddToMap", {boolean(true)}, false),
                              method_call("visible", "IsMapMarkerVisible", {}, true),
                              method_call("travel", "CanFastTravelToMarker", {}, true),
                              method_call("notravel", "EnableFastTravel", {boolean(false)}, false)}));
    const auto marker = rig.vm.object(k_marker, "ObjectReference");
    CHECK_FALSE(rig.call("caller", "visible", {marker}).b());
    rig.call("caller", "add", {marker});
    CHECK(rig.call("caller", "visible", {marker}).b());
    CHECK_FALSE(rig.call("caller", "travel", {marker}).b());
    rig.call("caller", "addtravel", {marker});
    CHECK(rig.call("caller", "travel", {marker}).b());
    CHECK(*skydot::vm::map_marker_flags(rig.vm, data_flags, k_marker) == 3);
    rig.call("caller", "notravel", {marker});
    CHECK(*skydot::vm::map_marker_flags(rig.vm, data_flags, k_marker) == 1);

    // The data's flags until something changes them; a reference that is no marker stays none.
    const auto capital = rig.vm.object(k_capital, "ObjectReference");
    CHECK(rig.call("caller", "visible", {capital}).b());
    CHECK_FALSE(rig.call("caller", "travel", {capital}).b());
    const auto rock = rig.vm.object(k_rock, "ObjectReference");
    rig.call("caller", "addtravel", {rock});
    CHECK_FALSE(rig.call("caller", "visible", {rock}).b());
    CHECK_FALSE(skydot::vm::map_marker_flags(rig.vm, data_flags, k_rock).has_value());
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("Game.EnableFastTravel blocks fast travel and FastTravel leaves a request", "[vm][natives][map]") {
    Rig rig;
    add_classes(rig);
    skydot::vm::bind_map_natives(rig.vm, data_flags);
    auto off = global_function("off", {}, {{"r", "Bool"}}, "Bool");
    off.add(Op::callstatic, {ident("Game"), ident("EnableFastTravel"), ident("::NoneVar"), boolean(false)})
        .add(Op::callstatic, {ident("Game"), ident("IsFastTravelEnabled"), ident("r")})
        .add(Op::return_, {ident("r")});
    auto go = global_function("go", {{"o", "ObjectReference"}}, {}, "Bool");
    go.add(Op::callstatic, {ident("Game"), ident("FastTravel"), ident("::NoneVar"), ident("o")})
        .add(Op::return_, {boolean(true)});
    rig.add(script("Caller", {off, go}));

    CHECK(rig.vm.map().fast_travel_enabled);
    CHECK_FALSE(rig.call("caller", "off").b());
    CHECK_FALSE(rig.vm.map().fast_travel_enabled);
    rig.call("caller", "go", {rig.vm.object(k_capital, "ObjectReference")});
    CHECK(rig.vm.map().travel_request == k_capital);
    CHECK(rig.vm.errors() == 0);
}

TEST_CASE("the map markers' flags and the fast travel block are part of the VM save", "[vm][save][map]") {
    skydot::vm::Vm a;
    a.map().set(k_marker, 3);
    a.map().set(k_capital, 3);
    a.map().fast_travel_enabled = false;
    a.map().travel_request = k_capital;
    const auto bytes = a.save();

    skydot::vm::Vm b;
    std::string error;
    REQUIRE(b.load(bytes, error));
    CHECK(b.map().states().size() == 2);
    CHECK(*b.map().find(k_marker) == 3);
    CHECK_FALSE(b.map().fast_travel_enabled);
    CHECK(b.map().travel_request == 0); // a request is taken the frame it is made, never saved

    // A VM cleared for a new game knows no changes.
    b.clear();
    CHECK(b.map().states().empty());
    CHECK(b.map().fast_travel_enabled);
}
