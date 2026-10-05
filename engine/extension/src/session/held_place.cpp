// SPDX-License-Identifier: GPL-3.0-or-later
#include "session/held_place.hpp"

#include <godot_cpp/classes/navigation_link3d.hpp>
#include <godot_cpp/classes/navigation_region3d.hpp>

namespace skydot::held_place {
namespace {

/// Every region and ledge link under `root`: a cell keeps them under a
/// "Navmesh" node, with the links below their region.
void set_navigation(godot::Node3D* root, bool enabled) {
    for (const auto& found : root->find_children("*", "NavigationRegion3D", true, false)) {
        if (auto* region = godot::Object::cast_to<godot::NavigationRegion3D>(found)) {
            region->set_enabled(enabled);
        }
    }
    for (const auto& found : root->find_children("*", "NavigationLink3D", true, false)) {
        if (auto* link = godot::Object::cast_to<godot::NavigationLink3D>(found)) {
            link->set_enabled(enabled);
        }
    }
}

} // namespace

void hold(godot::Node* host, godot::Node3D* node) {
    node->set_visible(false);
    node->set_process_mode(godot::Node::PROCESS_MODE_DISABLED);
    set_navigation(node, false);
    host->add_child(node);
}

void release(godot::Node3D* node) {
    node->set_process_mode(godot::Node::PROCESS_MODE_INHERIT);
    node->set_visible(true);
    set_navigation(node, true);
}

} // namespace skydot::held_place
