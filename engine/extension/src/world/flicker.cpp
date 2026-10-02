// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/flicker.hpp"

#include <godot_cpp/classes/light3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <cmath>
#include <numbers>

namespace skydot {

namespace {

double hash(double x) {
    const double s = std::sin(x * 127.1 + 311.7) * 43758.5453;
    return s - std::floor(s);
}

/// Smooth value noise in [0, 1].
double noise(double x) {
    const double i = std::floor(x);
    const double f = x - i;
    const double u = f * f * (3.0 - 2.0 * f);
    return hash(i) + (hash(i + 1.0) - hash(i)) * u;
}

} // namespace

void SkydotFlicker::_bind_methods() {
    using godot::D_METHOD;
    godot::ClassDB::bind_method(D_METHOD("configure", "flags", "period", "intensity", "movement"),
                                &SkydotFlicker::configure);
    godot::ClassDB::bind_method(D_METHOD("factor_at", "seconds"), &SkydotFlicker::factor_at);
    godot::ClassDB::bind_method(D_METHOD("offset_at", "seconds"), &SkydotFlicker::offset_at);
}

namespace {
constexpr double k_flicker_movement = 8.0;
} // namespace

void SkydotFlicker::configure(std::int64_t flags, double period, double intensity,
                              double movement) {
    flags_ = static_cast<std::uint32_t>(flags);
    const bool slow = (flags_ & (FLICKER_SLOW | PULSE_SLOW)) != 0;
    period_ = period > 0.0 ? period : (slow ? 1.0 : 0.2);
    intensity_ = intensity;
    movement_ = (flags_ & (FLICKER | FLICKER_SLOW)) != 0 ? movement : 0.0;
}

double SkydotFlicker::factor_at(double seconds) const {
    const double x = seconds / period_ + seed_;
    if ((flags_ & (PULSE | PULSE_SLOW)) != 0) {
        return 1.0 + intensity_ * std::sin(2.0 * std::numbers::pi * x);
    }
    return 1.0 + intensity_ * (2.0 * noise(x) - 1.0);
}

godot::Vector3 SkydotFlicker::offset_at(double seconds) const {
    if (movement_ <= 0.0) {
        return {};
    }
    const double x = seconds / period_ + seed_;
    const godot::Vector3 wander(static_cast<float>(2.0 * noise(x + 17.0) - 1.0),
                                static_cast<float>(2.0 * noise(x + 43.0) - 1.0),
                                static_cast<float>(2.0 * noise(x + 71.0) - 1.0));
    // The Creation Kit scales the amplitude by the setting fFlickerMovement,
    // which no plugin sets; its known default (Oblivion's) is 8. Read as a
    // divisor, a torch's 48 units move it within 6 (9 cm): the game's torches
    // stay in their sconces. Not checked against the game.
    return wander.limit_length(1.0F) * static_cast<float>(movement_ / k_flicker_movement);
}

void SkydotFlicker::_ready() {
    if (auto* light = godot::Object::cast_to<godot::Light3D>(get_parent())) {
        base_energy_ = static_cast<double>(light->get_param(godot::Light3D::PARAM_ENERGY));
        base_position_ = light->get_position();
        // Neighbouring lights should not flicker in step.
        const godot::Vector3 p = base_position_;
        seed_ = hash(static_cast<double>(p.x) * 0.37 + static_cast<double>(p.y) * 1.3 +
                     static_cast<double>(p.z) * 2.1) *
                1000.0;
    }
    set_process(flags_ != 0);
}

void SkydotFlicker::_process(double /*delta*/) {
    auto* light = godot::Object::cast_to<godot::Light3D>(get_parent());
    if (light == nullptr) {
        return;
    }
    const double now = static_cast<double>(godot::Time::get_singleton()->get_ticks_usec()) / 1e6;
    light->set_param(godot::Light3D::PARAM_ENERGY,
                     static_cast<float>(std::max(base_energy_ * factor_at(now), 0.0)));
    if (movement_ > 0.0) {
        light->set_position(base_position_ + offset_at(now));
    }
}

} // namespace skydot
