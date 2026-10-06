// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which large references (world.fb format 11, formats/pack-format.md "large
// references") are drawn around a camera: the pure choice, apart from any
// Godot type, so it is testable on its own.
//
// In the game the cells at full detail build their references, and beyond
// them, out to uLargeRefLODGridSize, the large references draw as models. A
// cell's list holds the large references standing in it and those of its
// neighbours whose bounds reach into it, so the union of the lists of the
// cells in range, each reference once, is everything visible there.
#pragma once

#include "data/world_data.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace skydot::large_refs {

/// Whether the grid square (x, y) is built at full detail.
using CellBuilt = std::function<bool(std::int32_t x, std::int32_t y)>;

/// The large references of `world` to draw for a camera in square (cx, cy):
/// the union of the lists of the squares within `radius` (Chebyshev), as
/// indices into the worldspace's `large_refs`, ascending and each once, but
/// not those standing in a square `built` says is built: that cell draws
/// them. Squares inside the full-detail radius are in the union on purpose, so
/// a reference stays drawn until its own cell has been built.
std::vector<std::uint32_t> select(const WorldData& data, std::uint32_t world, std::int32_t cx,
                                  std::int32_t cy, std::int32_t radius, const CellBuilt& built);

/// Whether a camera cell (cx, cy) is within `radius` squares of (x, y).
inline bool within(std::int32_t cx, std::int32_t cy, std::int32_t x, std::int32_t y, std::int32_t radius) {
    const auto dx = cx > x ? cx - x : x - cx;
    const auto dy = cy > y ? cy - y : y - cy;
    return (dx > dy ? dx : dy) <= radius;
}

} // namespace skydot::large_refs
