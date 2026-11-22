// SPDX-License-Identifier: GPL-3.0-or-later
//
// World map natives: the map markers' flags (XMRK FNAM: 1 visible, 2 can travel to) and fast travel.
//   ObjectReference.AddToMap(abAllowFastTravel = false)   visible, and with the flag also travel
//   ObjectReference.EnableFastTravel(abEnable = true)     sets or clears "can travel to"
//   ObjectReference.IsMapMarkerVisible / CanFastTravelToMarker
//   Game.EnableFastTravel(abEnable = true) / IsFastTravelEnabled   the script block (sNoFastTravelScriptBlock)
//   Game.FastTravel(akDestination)   a request the game layer takes (SkydotPapyrus.take_fast_travel_request)
// Names and defaults from the Creation Kit wiki's ObjectReference and Game script pages; what the flags mean from
// skycompare-data/map-research.md (measured with SkyrimRemote @marker: discovery sets 0 -> 3).
#include "vm/natives_args.hpp"
#include "vm/natives_families.hpp"

namespace skydot::vm {

namespace {

constexpr std::uint8_t k_visible = 0x1;
constexpr std::uint8_t k_can_travel = 0x2;

} // namespace

std::optional<std::uint8_t> map_marker_flags(const Vm& vm, const MarkerFlagsSource& data, std::uint32_t ref) {
    if (const std::uint8_t* set = vm.map().find(ref)) {
        return *set;
    }
    return data ? data(ref) : std::nullopt;
}

void bind_map_natives(Vm& v, MarkerFlagsSource data) {
    using namespace natives;
    // The flags of a reference that is a map marker; a reference that is none changes nothing and reads false.
    const auto change = [data](NativeCall& c, std::uint8_t set, std::uint8_t clear) {
        const auto flags = map_marker_flags(c.vm, data, c.self.form());
        if (flags.has_value()) {
            c.vm.map().set(c.self.form(), static_cast<std::uint8_t>((*flags | set) & ~clear));
        }
    };
    v.bind("ObjectReference", "AddToMap", [change](NativeCall& c) {
        const bool travel = arg(c, 0).kind() == Kind::boolean && arg(c, 0).b();
        change(c, static_cast<std::uint8_t>(k_visible | (travel ? k_can_travel : 0)), 0);
        return NativeResult{};
    });
    v.bind("ObjectReference", "EnableFastTravel", [change](NativeCall& c) {
        const bool enable = arg(c, 0).kind() != Kind::boolean || arg(c, 0).b();
        change(c, enable ? k_can_travel : 0, enable ? 0 : k_can_travel);
        return NativeResult{};
    });
    v.bind("ObjectReference", "IsMapMarkerVisible", [data](NativeCall& c) {
        const auto flags = map_marker_flags(c.vm, data, c.self.form());
        return result(Value::boolean(flags.has_value() && (*flags & k_visible) != 0));
    });
    v.bind("ObjectReference", "CanFastTravelToMarker", [data](NativeCall& c) {
        const auto flags = map_marker_flags(c.vm, data, c.self.form());
        return result(Value::boolean(flags.has_value() && (*flags & k_can_travel) != 0));
    });
    v.bind("Game", "EnableFastTravel", [](NativeCall& c) {
        c.vm.map().fast_travel_enabled = arg(c, 0).kind() != Kind::boolean || arg(c, 0).b();
        return NativeResult{};
    });
    v.bind("Game", "IsFastTravelEnabled",
           [](NativeCall& c) { return result(Value::boolean(c.vm.map().fast_travel_enabled)); });
    v.bind("Game", "FastTravel", [](NativeCall& c) {
        if (arg(c, 0).kind() == Kind::object && arg(c, 0).form() != 0) {
            c.vm.map().travel_request = arg(c, 0).form();
        }
        return NativeResult{};
    });
}

} // namespace skydot::vm
