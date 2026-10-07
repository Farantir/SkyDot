// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sun's path over the day, as the game places it (measured in Skyrim SE
// with SkyrimRemote's `@sun`, SkyrimClimate and BlackreachClimate,
// 2026-10-07):
//
// - It rises 15 minutes before the middle of the climate's sunrise and sets
//   15 minutes after the middle of its sunset (SkyrimClimate: 7:30 and
//   18:30). Noon is half-way between.
// - x runs linearly from 1 (east) at rise to -1 (west) at set, and back over
//   the night.
// - The disc sits on a diamond, (x, 1 - |x|) in the east-up plane, below the
//   horizon at night, 0.1 towards the north.
// - The light that shades the world is not the disc: (x, 0.75 up, 0.1
//   north), so it never drops below about 37 degrees and still shines from
//   above at night (as the moon's light).
//
// Godot space: east +X, up +Y, north -Z.
#pragma once

#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cmath>

namespace skydot {

struct SunPath {
    godot::Vector3 light; ///< towards the light that shades the world
    godot::Vector3 disc;  ///< towards the sun's disc
    bool up = false;      ///< the disc is above the horizon
};

/// `times`: the climate's sunrise begin and end, sunset begin and end (hours).
inline SunPath sun_path(float hour, const float times[4]) {
    const float rise = (times[0] + times[1]) * 0.5F - 0.25F;
    const float set = std::max((times[2] + times[3]) * 0.5F + 0.25F, rise + 0.1F);
    const float day_length = set - rise;
    const float h = std::fmod(std::fmod(hour, 24.0F) + 24.0F, 24.0F);
    SunPath out;
    float x = 0.0F;
    if (h >= rise && h <= set) {
        out.up = true;
        x = 1.0F - 2.0F * (h - rise) / day_length;
    } else {
        const float night_length = std::max(24.0F - day_length, 0.1F);
        x = -1.0F + 2.0F * std::fmod(h - set + 24.0F, 24.0F) / night_length;
    }
    const float height = 1.0F - std::abs(x);
    out.light = godot::Vector3(x, 0.75F, -0.1F).normalized();
    out.disc = godot::Vector3(x, out.up ? height : -height, -0.1F).normalized();
    return out;
}

} // namespace skydot
