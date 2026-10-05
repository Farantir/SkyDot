// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotFlicker`: animates its parent light as LIGH's flicker and pulse flags
// ask: brightness varies by the intensity amplitude over the flicker period
// (smooth noise for flicker, a sine for pulse), and flickering lights wander
// within a 64th of the movement amplitude of their place.
#pragma once

#include "world_generated.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

namespace skydot {

class SkydotFlicker : public godot::Node {
    GDCLASS(SkydotFlicker, godot::Node)

public:
    /// The LIGH flags that make a light flicker or pulse.
    static constexpr bethconv::pack::wfb::LightFlags ANY =
        bethconv::pack::wfb::LightFlags::flicker | bethconv::pack::wfb::LightFlags::flicker_slow |
        bethconv::pack::wfb::LightFlags::pulse | bethconv::pack::wfb::LightFlags::pulse_slow;

    /// `flags` are the light's LightFlags; `movement` is in metres.
    void configure(std::int64_t flags, double period, double intensity, double movement);

    /// Brightness factor and offset at `seconds`; also used by tests.
    double factor_at(double seconds) const;
    godot::Vector3 offset_at(double seconds) const;

    void _ready() override;
    void _process(double delta) override;

protected:
    static void _bind_methods();

private:
    bethconv::pack::wfb::LightFlags flags_{};
    double period_ = 0.2;
    double intensity_ = 0.0;
    double movement_ = 0.0;
    double seed_ = 0.0;
    double base_energy_ = 1.0;
    godot::Vector3 base_position_;
};

} // namespace skydot
