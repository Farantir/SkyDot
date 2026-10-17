// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/switch_node.hpp"

#include "render/effect_asset.hpp"

#include <godot_cpp/classes/collision_object3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <algorithm>

namespace skydot {

namespace {

constexpr int k_max_depth = 64;

/// The node's `bethconv.switch` extra, or -1.
std::int64_t switch_extra(const godot::Node* node) {
    const godot::Variant value = bethconv_extras(node).get("switch", godot::Variant());
    return value.get_type() == godot::Variant::INT || value.get_type() == godot::Variant::FLOAT
               ? static_cast<std::int64_t>(value)
               : -1;
}

/// A child the switch chooses between: not the collision bodies and trigger
/// volumes the converter's extras attach to the same node.
godot::Node3D* model_child(godot::Node* node, std::int32_t i) {
    godot::Node* child = node->get_child(i);
    if (godot::Object::cast_to<godot::CollisionObject3D>(child) != nullptr) {
        return nullptr;
    }
    return godot::Object::cast_to<godot::Node3D>(child);
}

/// Shows child `index` of `node`, hides the others; `index < 0` keeps its own.
std::int64_t walk(godot::Node* node, std::int64_t index, int depth) {
    if (depth > k_max_depth) {
        return 0;
    }
    std::int64_t count = 0;
    const std::int64_t own = switch_extra(node);
    if (own >= 0) {
        ++count;
        const std::int64_t shown = index >= 0 ? index : own;
        std::int64_t position = 0;
        std::int64_t models = 0;
        for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
            models += model_child(node, i) != nullptr ? 1 : 0;
        }
        const std::int64_t last = std::max<std::int64_t>(models - 1, 0);
        for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
            if (auto* child = model_child(node, i)) {
                child->set_visible(position == std::min(shown, last));
                ++position;
            }
        }
    }
    for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
        count += walk(node->get_child(i), index, depth + 1);
    }
    return count;
}

std::int64_t first_shown(godot::Node* node, int depth) {
    if (depth > k_max_depth) {
        return -1;
    }
    if (switch_extra(node) >= 0) {
        std::int64_t position = 0;
        for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
            if (const auto* child = model_child(node, i)) {
                if (child->is_visible()) {
                    return position;
                }
                ++position;
            }
        }
        return -1;
    }
    for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
        if (const std::int64_t found = first_shown(node->get_child(i), depth + 1); found >= 0) {
            return found;
        }
    }
    return -1;
}

} // namespace

void SkydotSwitch::_bind_methods() {
    using godot::ClassDB;
    using godot::D_METHOD;
    ClassDB::bind_static_method("SkydotSwitch", D_METHOD("apply_defaults", "root"), &SkydotSwitch::apply_defaults);
    ClassDB::bind_static_method("SkydotSwitch", D_METHOD("set_index", "root", "index"), &SkydotSwitch::set_index);
    ClassDB::bind_static_method("SkydotSwitch", D_METHOD("get_index", "root"), &SkydotSwitch::get_index);
}

std::int64_t SkydotSwitch::apply_defaults(godot::Node* root) {
    return root != nullptr ? walk(root, -1, 0) : 0;
}

std::int64_t SkydotSwitch::set_index(godot::Node* root, std::int64_t index) {
    return root != nullptr ? walk(root, std::max<std::int64_t>(index, 0), 0) : 0;
}

std::int64_t SkydotSwitch::get_index(godot::Node* root) {
    return root != nullptr ? first_shown(root, 0) : -1;
}

} // namespace skydot
