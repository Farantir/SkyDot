// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/shadow_budget.hpp"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/light3d.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace skydot {

namespace {
// How often the choice is made again, in seconds.
constexpr double k_interval = 0.2;
// A light with a shadow counts as this much nearer (metres) when they are
// ranked, so the choice does not flip between two lights about as far.
constexpr float k_keep = 1.0F;
} // namespace

void SkydotShadowBudget::_bind_methods() {
    using godot::D_METHOD;
    godot::ClassDB::bind_method(D_METHOD("set_limit", "limit"), &SkydotShadowBudget::set_limit);
    godot::ClassDB::bind_method(D_METHOD("get_limit"), &SkydotShadowBudget::get_limit);
    godot::ClassDB::bind_method(D_METHOD("set_active", "active"), &SkydotShadowBudget::set_active);
    godot::ClassDB::bind_method(D_METHOD("is_active"), &SkydotShadowBudget::is_active);
    godot::ClassDB::bind_method(D_METHOD("update_from", "position"), &SkydotShadowBudget::update_from);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::INT, "limit"), "set_limit", "get_limit");
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::BOOL, "active"), "set_active", "is_active");
}

void SkydotShadowBudget::set_active(bool active) {
    active_ = active;
    since_ = 1e9;
}

std::int64_t SkydotShadowBudget::update_from(const godot::Vector3& position) {
    auto* root = get_parent();
    if (root == nullptr || !is_inside_tree()) {
        return 0;
    }
    std::vector<std::pair<float, godot::Light3D*>> ranked;
    const godot::TypedArray<godot::Node> nodes = get_tree()->get_nodes_in_group(GROUP);
    for (int64_t i = 0; i < nodes.size(); ++i) {
        auto* light = godot::Object::cast_to<godot::Light3D>(nodes[i]);
        if (light == nullptr || !root->is_ancestor_of(light)) {
            continue;
        }
        if (!light->is_visible_in_tree()) {
            light->set_shadow(false);
            continue;
        }
        float distance = light->get_global_position().distance_to(position);
        if (light->has_shadow()) {
            distance -= k_keep;
        }
        ranked.emplace_back(distance, light);
    }
    const auto keep = static_cast<std::size_t>(std::max<std::int64_t>(limit_, 0));
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::int64_t shadowed = 0;
    for (std::size_t i = 0; i < ranked.size(); ++i) {
        const bool shadow = i < keep;
        if (ranked[i].second->has_shadow() != shadow) {
            ranked[i].second->set_shadow(shadow);
        }
        shadowed += shadow ? 1 : 0;
    }
    return shadowed;
}

void SkydotShadowBudget::_ready() {
    set_process(true);
}

void SkydotShadowBudget::_process(double delta) {
    if (!active_) {
        return;
    }
    since_ += delta;
    if (since_ < k_interval) {
        return;
    }
    since_ = 0.0;
    const auto* camera = get_viewport() != nullptr ? get_viewport()->get_camera_3d() : nullptr;
    if (camera != nullptr) {
        update_from(camera->get_global_position());
    }
}

} // namespace skydot
