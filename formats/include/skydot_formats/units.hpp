// SPDX-License-Identifier: GPL-3.0-or-later
//
// The units of Skyrim's space, which the pack keeps as the game does (Z-up,
// game units; see pack-format.md). The converter scales every mesh by
// `k_metres_per_unit` and the engine every position, so the two must read one
// number.
#pragma once

namespace skydot::formats {

/// Metres per game unit: 64 units to a yard.
inline constexpr double k_metres_per_unit = 0.0142875;

/// Game units along the side of an exterior cell; the grid square of a position
/// is `floor(position / k_cell_units)`.
inline constexpr int k_cell_units = 4096;

/// Game units per Havok unit: a Havok length times this is in the units of the
/// node the body hangs on.
inline constexpr double k_havok_scale = 69.99124;

// Where a float is wanted, it is the float literal the code used before.
static_assert(static_cast<float>(k_metres_per_unit) == 0.0142875F);
static_assert(static_cast<float>(k_havok_scale) == 69.99124F);

} // namespace skydot::formats
