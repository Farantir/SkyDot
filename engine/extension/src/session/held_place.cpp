// SPDX-License-Identifier: GPL-3.0-or-later
#include "session/held_place.hpp"

#include <godot_cpp/classes/navigation_region3d.hpp>

namespace skydot::held_place {
namespace {

/// Only `root`'s own children, as the viewer's HeldPlace did. A cell keeps its
/// regions under a "Navmesh" node, so those are not reached: they stay on the
/// navigation map while the cell is held.
void set_regions(godot::Node3D* root, bool enabled) {
    for (std::int32_t i = 0; i < root->get_child_count(); ++i) {
        if (auto* region = godot::Object::cast_to<godot::NavigationRegion3D>(root->get_child(i))) {
            region->set_enabled(enabled);
        }
    }
}

} // namespace

void hold(godot::Node* host, godot::Node3D* node) {
    node->set_visible(false);
    node->set_process_mode(godot::Node::PROCESS_MODE_DISABLED);
    host->add_child(node);
    set_regions(node, false);
}

void release(godot::Node3D* node) {
    node->set_process_mode(godot::Node::PROCESS_MODE_INHERIT);
    node->set_visible(true);
    set_regions(node, true);
}

} // namespace skydot::held_place
