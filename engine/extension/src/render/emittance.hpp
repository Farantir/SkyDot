// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotEmittance`: tints its parent light with the Effect Lighting colour
// (NAM0 index 9) of a region's weather, as a reference's XEMI asks (REFR XEMI
// names a REGN; the pack lists it as a `LightEmitter`). The Whiterun street
// fires name FXWthrInvertLightsWhiterun: dim orange by day (83/60/34), bright
// orange at night (238/184/70). Its sunlight colour is black by day, but the
// game's brazier light is on by day: disabling it there takes 13/6/0 off the
// whole frame at 13:12 (34/21/10 off the wall next to it), and SkyDot's light
// times Effect Lighting takes 16/8/1 (34/18/4); at 22:00 the game 20/14/2,
// SkyDot 27/19/5 (game-refs-2026-10-11, brazier_toggle.txt). The light is its
// own colour times that colour; with no weather running (an interior without
// sky) it stays as it is. A show-sky interior's weather counts.
//
// Under a node that is no light (an effect whose XEMI names a region) it sets
// the lit effect shaders' `effect_emittance` on the shapes below that node
// instead (effect.gdshaderinc). When the running weather is one of the
// region's, its colour is the running weather's: Bleak Falls Barrow's light
// beams (XEMI WeatherMountains, its own sky region) add 0.553:1:1.146 of
// green in the game at 4:44 under SkyrimCloudySN, whose night Effect Lighting
// is 84/148/169 (0.568:1:1.142); the region's chance-weighted mean is
// 0.74:1:1.04.
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

    /// The tint now: the running weather's Effect Lighting colour if it is
    /// one of the region's, else the chance-weighted mean of the weathers'
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
