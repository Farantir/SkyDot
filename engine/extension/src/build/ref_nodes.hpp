// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which node stands for a reference: every model `tag_ref` marks is
// registered here and removed as its node leaves the tree (or is freed), so
// finding a reference's node, and asking whether it is loaded, is a lookup and
// not a walk over the scene. A reference built twice (a cell streamed out and
// in) answers with the newest node.
#pragma once

#include <godot_cpp/classes/node.hpp>

#include <cstdint>

namespace skydot::ref_nodes {

void add(std::uint32_t ref, godot::Node* node);
/// The live node of `ref`, or null: unbuilt, freed or about to be.
godot::Node* find(std::uint32_t ref);
/// Forget everything (tests).
void clear();

} // namespace skydot::ref_nodes
