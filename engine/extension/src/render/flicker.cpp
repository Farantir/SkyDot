// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/flicker.hpp"

#include "skydot_formats/flags.hpp"

#include <godot_cpp/classes/light3d.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace skydot {

namespace {

namespace wfb = bethconv::pack::wfb;

constexpr auto k_slow = wfb::LightFlags::flicker_slow | wfb::LightFlags::pulse_slow;
constexpr auto k_pulsing = wfb::LightFlags::pulse | wfb::LightFlags::pulse_slow;
constexpr auto k_wandering = wfb::LightFlags::flicker | wfb::LightFlags::flicker_slow;

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
    godot::ClassDB::bind_method(D_METHOD("configure", "flags", "inverse_period", "intensity", "movement", "fade"),
                                &SkydotFlicker::configure, DEFVAL(1.0));
    godot::ClassDB::bind_method(D_METHOD("factor_at", "seconds"), &SkydotFlicker::factor_at);
    godot::ClassDB::bind_method(D_METHOD("offset_at", "seconds"), &SkydotFlicker::offset_at);
}

namespace {
constexpr double k_flicker_movement = 64.0;
} // namespace

void SkydotFlicker::configure(std::int64_t flags, double inverse_period, double intensity,
                              double movement, double fade) {
    flags_ = static_cast<wfb::LightFlags>(flags);
    const bool slow = formats::has_flag(flags_, k_slow);
    // LIGH stores 1/period (UESP, Mod File Format/LIGH): the Whiterun street
    // fires' 0.05 is a 20 s cycle. Read as seconds it made them jump every
    // frame (comparison series 194712 at night, 2026-10-07).
    period_ = inverse_period > 0.0 ? 1.0 / inverse_period : (slow ? 1.0 : 0.2);
    // The intensity amplitude is how far the light dims below its fade (the
    // Creation Kit's Light page), so relative to the fade, and never brighter.
    depth_ = std::clamp(intensity / (fade > 0.0 ? fade : 1.0), 0.0, 1.0);
    movement_ = formats::has_flag(flags_, k_wandering) ? movement : 0.0;
}

double SkydotFlicker::factor_at(double seconds) const {
    const double x = seconds / period_ + seed_;
    if (formats::has_flag(flags_, k_pulsing)) {
        return 1.0 - depth_ * (0.5 + 0.5 * std::sin(2.0 * std::numbers::pi * x));
    }
    return 1.0 - depth_ * noise(x);
}

godot::Vector3 SkydotFlicker::offset_at(double seconds) const {
    if (movement_ <= 0.0) {
        return {};
    }
    const double x = seconds / period_ + seed_;
    const godot::Vector3 wander(static_cast<float>(2.0 * noise(x + 17.0) - 1.0),
                                static_cast<float>(2.0 * noise(x + 43.0) - 1.0),
                                static_cast<float>(2.0 * noise(x + 71.0) - 1.0));
    // The game's scale for the amplitude is unknown (fFlickerMovement, which
    // no plugin sets). Oblivion's default 8 as a divisor moved a torch's 48
    // units within 9 cm, and a lantern's light swung the shadows of its own
    // cage across the scene (user report, 2026-10-04). 64 keeps it within
    // about 1 cm. Not measured in the game.
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
    set_process(flags_ != wfb::LightFlags::NONE);
}

void SkydotFlicker::_process(double delta) {
    auto* light = godot::Object::cast_to<godot::Light3D>(get_parent());
    if (light == nullptr) {
        return;
    }
    // Engine time, not the wall clock: a frame series rendered at a fixed
    // frame rate then shows the flicker at its real speed.
    elapsed_ += delta;
    const double now = elapsed_;
    light->set_param(godot::Light3D::PARAM_ENERGY,
                     static_cast<float>(std::max(base_energy_ * factor_at(now), 0.0)));
    if (movement_ > 0.0) {
        light->set_position(base_position_ + offset_at(now));
    }
}

} // namespace skydot
