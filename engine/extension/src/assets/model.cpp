// SPDX-License-Identifier: GPL-3.0-or-later
#include "assets/model.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>

namespace skydot {

void SkydotModel::_bind_methods() {
    using godot::ClassDB;
    using godot::D_METHOD;
    ClassDB::bind_method(D_METHOD("instantiate"), &SkydotModel::instantiate);
    ClassDB::bind_method(D_METHOD("get_vpath"), &SkydotModel::get_vpath);
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

} // namespace skydot
