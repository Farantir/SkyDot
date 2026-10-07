// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotEmittance`: tints its parent light with the sunlight colour of a
// region's weather, as a reference's XEMI asks (REFR XEMI names a REGN; the
// pack lists it as a `LightEmitter`). Vanilla switches street fires and window
// lights with the time of day this way: the region's only weather has a black
// sunlight colour by day and a white one at night, so the light is dark by day
// and burns at dusk (the Whiterun brazier, shot 2026-10-06). The cave weathers
// carry coloured sunlight instead, which tints the light. The light is its own
// colour times the weather's sunlight colour; with no weather running (an
// interior) it stays as it is.
#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>

#include <cstdint>
#include <vector>

namespace skydot {

class SkydotFlicker;

class SkydotEmittance : public godot::Node {
    GDCLASS(SkydotEmittance, godot::Node)

public:
    /// `gamma` is the light's colour times its fade (what the game adds to a
    /// surface, see SkydotMaterials.set_game_light); `weathers` and `chances`
    /// are the region's weather list.
    void configure(const godot::Color& gamma, const godot::PackedInt64Array& weathers,
                   const godot::PackedInt64Array& chances);

    /// The tint now: the chance-weighted mean of the weathers' sunlight colour
    /// at the running time of day, or white if no weather runs.
    godot::Color tint();

    void _ready() override;
    void _process(double delta) override;

protected:
    static void _bind_methods();

private:
    void apply();

    godot::Color gamma_;
    std::vector<std::pair<std::int64_t, std::int64_t>> weathers_;
    double since_ = 1e9;
    godot::Color applied_{-1, -1, -1, -1};
};

} // namespace skydot
