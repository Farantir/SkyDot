// SPDX-License-Identifier: GPL-3.0-or-later
//
// A position a furniture model offers actors (the converter's `bethconv.furniture_markers` extra, from
// the NIF's BSFurnitureMarkerNode). No Godot in here.
#pragma once

#include <array>
#include <cstdint>

namespace skydot {

struct FurnitureMarker {
    /// NIF units, in the model root's space (z up).
    std::array<float, 3> offset{};
    /// Radians.
    float heading{};
    /// 1 sit, 2 sleep, 3 lean.
    std::uint16_t animation_type{};
    /// Bit mask of the sides it can be entered from: 0x1 front, 0x2 behind, 0x4 right, 0x8 left, 0x10 up.
    std::uint16_t entry_points{};
};

} // namespace skydot
