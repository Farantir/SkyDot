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

/// One moon's place in the sky, as the game puts it (measured in Skyrim SE
/// with SkyrimRemote's `@sun`, 2026-10-10; the parameters are the game's INI
/// defaults, read back from Sky::masser / Sky::secunda):
///
/// - The moon turns on a small circle about the east axis, `z_offset`
///   degrees east of the north-up plane: (sin z, cos z cos a, cos z sin a)
///   in east-north-up, so it rises in the north at a = 0, culminates over
///   the east at a = 90 and sets in the south at a = 180.
/// - a = 60 x speed degrees per game hour since day 0, 0:00, plus 90
///   (Masser 15 per hour, the same every day; Secunda 18, 72 later each
///   day).
/// - Its alpha ramps from 0 to 1 while a (or 180 - a) goes from
///   `fade_end` to `fade_start`; below the horizon it is not drawn. It is
///   drawn by day too, in the weather's moon colour. Its texture's up is the
///   way it moves.
struct MoonPath {
    godot::Vector3 direction; ///< towards the moon, Godot space
    godot::Vector3 up;        ///< the texture's up: the way the moon moves
    float alpha = 0.0F;       ///< 0 = not drawn
};

struct MoonParams {
    float speed;      ///< fMasserSpeed / fSecundaSpeed
    float z_offset;   ///< fMasserZOffset / fSecundaZOffset, degrees
    float size;       ///< iMasserSize / iSecundaSize: half width at 512 units
    float fade_start; ///< fMasserAngleFadeStart, degrees
    float fade_end;   ///< fMasserAngleFadeEnd, degrees
};

inline constexpr MoonParams k_masser{0.25F, 35.0F, 90.0F, 35.0F, 20.0F};
inline constexpr MoonParams k_secunda{0.3F, 50.0F, 40.0F, 35.0F, 20.0F};

/// `game_hours`: hours since day 0, 0:00 (days passed x 24 + hour).
inline MoonPath moon_path(double game_hours, const MoonParams& p) {
    constexpr double k_pi = 3.14159265358979323846;
    const double degrees = std::fmod(std::fmod(60.0 * static_cast<double>(p.speed) * game_hours + 90.0, 360.0) + 360.0, 360.0);
    const double a = degrees * k_pi / 180.0;
    const double z = static_cast<double>(p.z_offset) * k_pi / 180.0;
    MoonPath out;
    // East-north-up (sin z, cos z cos a, cos z sin a) in Godot's axes.
    out.direction = godot::Vector3(static_cast<float>(std::sin(z)), static_cast<float>(std::cos(z) * std::sin(a)),
                                   static_cast<float>(-std::cos(z) * std::cos(a)));
    // The quad turns with the moon: its texture's up is the orbit's tangent,
    // (0, -sin a, cos a) east-north-up (fitted to a game shot of Masser at
    // 22:00, whose texture shows turned 45 degrees).
    out.up = godot::Vector3(0.0F, static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    if (degrees < 180.0) {
        const auto above = static_cast<float>(std::min(degrees, 180.0 - degrees));
        out.alpha = std::clamp((above - p.fade_end) / std::max(p.fade_start - p.fade_end, 1e-3F), 0.0F, 1.0F);
    }
    return out;
}

/// The moon quad's angular width in degrees (its half width `size` at the
/// sky's 512 units).
inline float moon_degrees(const MoonParams& p) {
    return 2.0F * std::atan(p.size / 512.0F) * 180.0F / 3.14159265F;
}

} // namespace skydot
