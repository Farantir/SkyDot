// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/emittance.hpp"

#include "render/flicker.hpp"
#include "render/materials.hpp"
#include "render/weather.hpp"

#include <godot_cpp/classes/light3d.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cmath>

namespace skydot {

namespace {

/// Real seconds between looks at the sky; at 20 game seconds a second the
/// colour moves a few percent a minute.
constexpr double k_interval = 0.5;
constexpr float k_dark = 0.002F;

} // namespace

void SkydotEmittance::_bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("configure", "gamma", "weathers", "chances"),
                                &SkydotEmittance::configure);
    godot::ClassDB::bind_method(godot::D_METHOD("tint"), &SkydotEmittance::tint);
}

void SkydotEmittance::configure(const godot::Color& gamma, const godot::PackedInt64Array& weathers,
                                const godot::PackedInt64Array& chances) {
    gamma_ = gamma;
    weathers_.clear();
    for (int64_t i = 0; i < weathers.size(); ++i) {
        weathers_.emplace_back(weathers[i], i < chances.size() ? std::max<int64_t>(chances[i], 0) : 1);
    }
}

godot::Color SkydotEmittance::tint() {
    auto* tree = is_inside_tree() ? get_tree() : nullptr;
    auto* weather = tree != nullptr
                        ? godot::Object::cast_to<SkydotWeather>(tree->get_first_node_in_group("skydot_weather"))
                        : nullptr;
    if (weather == nullptr || weathers_.empty()) {
        return {1, 1, 1, 1};
    }
    double r = 0;
    double g = 0;
    double b = 0;
    double total = 0;
    for (const auto& [id, chance] : weathers_) {
        // A list whose chances are all 0 still names its weathers.
        const double w = static_cast<double>(chance) + 1e-3;
        const godot::Color c = weather->effect_light_of(id);
        r += static_cast<double>(c.r) * w;
        g += static_cast<double>(c.g) * w;
        b += static_cast<double>(c.b) * w;
        total += w;
    }
    return {static_cast<float>(r / total), static_cast<float>(g / total), static_cast<float>(b / total), 1.0F};
}

void SkydotEmittance::_ready() {
    set_process(true);
}

void SkydotEmittance::_process(double delta) {
    since_ += delta;
    if (since_ >= k_interval) {
        since_ = 0.0;
        apply();
    }
}

void SkydotEmittance::apply() {
    auto* light = godot::Object::cast_to<godot::Light3D>(get_parent());
    if (light == nullptr) {
        return;
    }
    const godot::Color t = tint();
    if (std::abs(t.r - applied_.r) + std::abs(t.g - applied_.g) + std::abs(t.b - applied_.b) < 0.002F) {
        return;
    }
    applied_ = t;
    light->set_visible(std::max({t.r, t.g, t.b}) > k_dark);
    SkydotMaterials::set_game_light(light, godot::Color(gamma_.r * t.r, gamma_.g * t.g, gamma_.b * t.b));
    if (auto* flicker = godot::Object::cast_to<SkydotFlicker>(light->get_node_or_null("SkydotFlicker"))) {
        flicker->set_base_energy(static_cast<double>(light->get_param(godot::Light3D::PARAM_ENERGY)));
    }
}

} // namespace skydot
