// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotFlicker`: animates its parent light as LIGH's flicker and pulse flags
// ask: brightness varies by the intensity amplitude over the flicker period
// (smooth noise for flicker, a sine for pulse), and flickering lights wander
// within a 64th of the movement amplitude of their place.
#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

namespace skydot {

class SkydotFlicker : public godot::Node {
    GDCLASS(SkydotFlicker, godot::Node)

public:
    // LIGH DATA flags.
    static constexpr std::uint32_t FLICKER = 0x0008;
    static constexpr std::uint32_t FLICKER_SLOW = 0x0040;
    static constexpr std::uint32_t PULSE = 0x0080;
    static constexpr std::uint32_t PULSE_SLOW = 0x0100;
    static constexpr std::uint32_t ANY = FLICKER | FLICKER_SLOW | PULSE | PULSE_SLOW;

    /// `movement` is in metres.
    void configure(std::int64_t flags, double period, double intensity, double movement);

    /// Brightness factor and offset at `seconds`; also used by tests.
    double factor_at(double seconds) const;
    godot::Vector3 offset_at(double seconds) const;

    void _ready() override;
    void _process(double delta) override;

protected:
    static void _bind_methods();

private:
    std::uint32_t flags_ = 0;
    double period_ = 0.2;
    double intensity_ = 0.0;
    double movement_ = 0.0;
    double seed_ = 0.0;
    double base_energy_ = 1.0;
    godot::Vector3 base_position_;
};

} // namespace skydot
