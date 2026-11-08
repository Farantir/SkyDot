// SPDX-License-Identifier: GPL-3.0-or-later
//
// The positions a furniture model offers actors (the converter's
// `bethconv.furniture_markers` extra, from the NIF's BSFurnitureMarkerNode):
// where to stand or lie, which way to face, what to do there (sit, sleep,
// lean) and from which sides it can be entered.
#pragma once

#include "data/furniture_marker.hpp"

#include <godot_cpp/classes/node.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace skydot {

/// The markers of an instantiated model (see SkydotMaterials for how models are
/// built), in the NIF's order; empty if it is not furniture.
[[nodiscard]] std::vector<FurnitureMarker> furniture_markers(const godot::Node* model);

} // namespace skydot
