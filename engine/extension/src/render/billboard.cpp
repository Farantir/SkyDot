// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/billboard.hpp"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace skydot {

using godot::Basis;
using godot::D_METHOD;
using godot::Dictionary;
using godot::Node3D;
using godot::Variant;
using godot::Vector3;

namespace {

constexpr godot::real_t k_min_length = 1e-6f;

/// The node's `bethconv.billboard` extra, or -1.
std::int64_t billboard_mode(const godot::Node* node) {
    if (!node->has_meta("extras")) {
        return -1;
    }
    const Variant extras = node->get_meta("extras");
    if (extras.get_type() != Variant::DICTIONARY) {
        return -1;
    }
    const Variant block = Dictionary(extras).get("bethconv", Variant());
    if (block.get_type() != Variant::DICTIONARY) {
        return -1;
    }
    const Variant mode = Dictionary(block).get("billboard", Variant());
    return mode.get_type() == Variant::INT || mode.get_type() == Variant::FLOAT
               ? static_cast<std::int64_t>(mode)
               : -1;
}

} // namespace

void SkydotBillboard::_bind_methods() {
    godot::ClassDB::bind_static_method("SkydotBillboard", D_METHOD("attach", "root"),
                                       &SkydotBillboard::attach);
    godot::ClassDB::bind_method(D_METHOD("set_mode", "mode"), &SkydotBillboard::set_mode);
    godot::ClassDB::bind_method(D_METHOD("get_mode"), &SkydotBillboard::get_mode);
    godot::ClassDB::bind_method(D_METHOD("face_camera"), &SkydotBillboard::face_camera);
    ADD_PROPERTY(godot::PropertyInfo(Variant::INT, "mode"), "set_mode", "get_mode");
}

std::int64_t SkydotBillboard::attach(godot::Node* root) {
    if (root == nullptr) {
        return 0;
    }
    std::int64_t count = 0;
    if (auto* node = godot::Object::cast_to<Node3D>(root)) {
        const std::int64_t mode = billboard_mode(node);
        if (mode >= 0) {
            auto* billboard = memnew(SkydotBillboard);
            billboard->set_name("SkydotBillboard");
            billboard->set_mode(mode);
            node->add_child(billboard);
            ++count;
        }
    }
    for (std::int32_t i = 0; i < root->get_child_count(); ++i) {
        godot::Node* child = root->get_child(i);
        if (godot::Object::cast_to<SkydotBillboard>(child) == nullptr) {
            count += attach(child);
        }
    }
    return count;
}

void SkydotBillboard::set_mode(std::int64_t mode) { mode_ = mode; }
std::int64_t SkydotBillboard::get_mode() const { return mode_; }

void SkydotBillboard::_ready() {
    if (auto* target = godot::Object::cast_to<Node3D>(get_parent())) {
        rest_ = target->get_transform();
    }
    set_process(true);
}

void SkydotBillboard::_process(double /*delta*/) { face_camera(); }

void SkydotBillboard::face_camera() {
    auto* target = godot::Object::cast_to<Node3D>(get_parent());
    godot::Viewport* viewport = get_viewport();
    godot::Camera3D* camera = viewport != nullptr ? viewport->get_camera_3d() : nullptr;
    if (target == nullptr || camera == nullptr) {
        return;
    }

    const Node3D* above = target->get_parent_node_3d();
    const Basis rest = above != nullptr ? above->get_global_basis() * rest_.basis : rest_.basis;
    const godot::Transform3D view = camera->get_global_transform();
    const Vector3 to_camera = view.origin - target->get_global_position();

    Basis facing = view.basis.orthonormalized();
    switch (mode_) {
    case ALWAYS_FACE_CENTER:
    case RIGID_FACE_CENTER:
        if (to_camera.length() > k_min_length) {
            const Vector3 z = to_camera.normalized();
            const Vector3 x = facing.get_column(1).cross(z);
            if (x.length() > k_min_length) {
                facing = Basis(x.normalized(), z.cross(x.normalized()), z);
            }
        }
        break;
    case ROTATE_ABOUT_UP:
    case BS_ROTATE_ABOUT_UP:
    case ROTATE_ABOUT_UP2: {
        const Vector3 up = rest.get_column(1).normalized();
        const Vector3 flat = to_camera - up * up.dot(to_camera);
        if (flat.length() <= k_min_length) {
            return;
        }
        const Vector3 z = flat.normalized();
        facing = Basis(up.cross(z), up, z);
        break;
    }
    default:
        break;
    }
    target->set_global_basis(facing.scaled_local(rest.get_scale()));
}

} // namespace skydot
