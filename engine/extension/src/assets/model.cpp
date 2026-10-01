// SPDX-License-Identifier: GPL-3.0-or-later
#include "assets/model.hpp"

#include "world/collision.hpp"

#include <godot_cpp/classes/node3d.hpp>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>

namespace skydot {

void SkydotModel::_bind_methods() {
    using godot::ClassDB;
    using godot::D_METHOD;
    ClassDB::bind_method(D_METHOD("instantiate"), &SkydotModel::instantiate);
    ClassDB::bind_method(D_METHOD("get_vpath"), &SkydotModel::get_vpath);
    ClassDB::bind_method(D_METHOD("attach_collision", "instance"), &SkydotModel::attach_collision);
    ClassDB::bind_method(D_METHOD("get_body_count"), &SkydotModel::get_body_count);
}

SkydotModel::~SkydotModel() {
    if (template_ != nullptr) {
        memdelete(template_);
    }
}

void SkydotModel::set_template(godot::Node* root, const godot::String& vpath) {
    if (template_ != nullptr) {
        memdelete(template_);
    }
    template_ = root;
    vpath_ = vpath;
}

godot::Node* SkydotModel::instantiate() const {
    if (template_ == nullptr) {
        return nullptr;
    }
    godot::Node* copy = template_->duplicate();
    if (copy != nullptr) {
        copy->set_scene_file_path(vpath_);
    }
    return copy;
}

std::int64_t SkydotModel::attach_collision(godot::Node* instance) const {
    auto* node = godot::Object::cast_to<godot::Node3D>(instance);
    return collision_ != nullptr && node != nullptr ? collision_->attach(node) : 0;
}

std::int64_t SkydotModel::get_body_count() const {
    return collision_ != nullptr ? static_cast<std::int64_t>(collision_->bodies().size()) : 0;
}

} // namespace skydot
