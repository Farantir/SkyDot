// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/flicker.hpp"

#include "skydot_formats/flags.hpp"
#include "skydot_formats/units.hpp"

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
// The wander picks a new direction this many times per flicker period.
// Fitted on the lantern light 0xB6246 (LIGH 0x2EE00: 1/period 0.25,
// movement 48; game-refs-2026-10-21 seq_212520{,_nolight}, 6 s): its share of
// the wall 69 units away swings 2-11 levels while the ground under it stays
// within +-1, and the frame's 8x8 cells vary by 9/15/21/27% (quartiles and
// 90th percentile) with frame steps of 3.8% per 0.1 s. The record's movement
// in units with 4 wanders per period gives 11/13/19/29% and 2.6%; once per
// period, 0.4%.
constexpr double k_wanders_per_period = 4.0;

// The drawn light follows its wander in steps of this many units (2.9 cm).
// Moving a Godot light costs render CPU every time (it is re-paired with the
// geometry in its range, and a shadowed light redraws its cube map): at
// Bleak Falls 181647 (45 flickering lights) moving them every frame cost
// 1.9 ms of an 11.3 ms frame. A wander of at most this amplitude is not drawn
// at all (the interior torches BleakFallsDraugrTorch01, movement 1, are the
// shadowed ones there); larger ones move every few frames, a step being
// smaller than the 3.8% per 0.1 s the lantern 0xB6246 changes in the game.
constexpr double k_drawn_step_units = 2.0;
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
    movement_ = formats::has_flag(flags_, k_wandering) &&
                        movement > k_drawn_step_units * formats::k_metres_per_unit + 1e-9
                    ? movement
                    : 0.0;
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
    const double x = seconds / period_ * k_wanders_per_period + seed_;
    const godot::Vector3 wander(static_cast<float>(2.0 * noise(x + 17.0) - 1.0),
                                static_cast<float>(2.0 * noise(x + 43.0) - 1.0),
                                static_cast<float>(2.0 * noise(x + 71.0) - 1.0));
    // The record's movement amplitude, in units (metres here). A 64th of it
    // (session 2026-10-04: a lantern swung the shadows of its own cage) was
    // from outdoor light shadows the game does not draw; the interior
    // torches move by 1 unit (BleakFallsDraugrTorch01).
    return wander.limit_length(1.0F) * static_cast<float>(movement_);
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
        const godot::Vector3 offset = offset_at(now);
        if (static_cast<double>(offset.distance_to(drawn_offset_)) >=
            k_drawn_step_units * formats::k_metres_per_unit) {
            drawn_offset_ = offset;
            light->set_position(base_position_ + offset);
        }
    }
}

} // namespace skydot
