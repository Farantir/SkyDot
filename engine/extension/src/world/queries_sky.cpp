// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/queries.hpp"
#include "data/fb_search.hpp"
#include "data/text.hpp"

#include "skydot_formats/units.hpp"
#include "world_generated.h"

#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using godot::Color;
using godot::Dictionary;
using godot::String;
using godot::Vector3;

namespace wfb = bethconv::pack::wfb;

namespace skydot::queries {

std::int64_t find_weather(const WorldData& data, const String& editor_id) {
    const auto* weathers = data.root() != nullptr ? data.root()->weathers() : nullptr;
    if (weathers == nullptr) {
        return 0;
    }
    const String wanted = editor_id.to_lower();
    for (const auto* w : *weathers) {
        if (to_godot(w->editor_id()).to_lower() == wanted) {
            return w->id();
        }
    }
    return 0;
}

Dictionary get_sky(const WorldData& data, std::int64_t world, double hour, std::int64_t weather_id) {
    Dictionary out;
    const auto* ws = data.world_ptr(world);
    const auto* climates = data.root() != nullptr ? data.root()->climates() : nullptr;
    const auto* weathers = data.root() != nullptr ? data.root()->weathers() : nullptr;
    if (ws == nullptr || climates == nullptr || weathers == nullptr) {
        return out;
    }
    const wfb::Climate* climate = lookup(climates, ws->climate());
    if (climate == nullptr && ws->parent() != 0) {
        if (const auto* parent = data.world_ptr(ws->parent())) {
            climate = lookup(climates, parent->climate());
        }
    }
    const wfb::Weather* weather =
        weather_id != 0 ? lookup(weathers, static_cast<std::uint32_t>(weather_id)) : nullptr;
    if (weather == nullptr && climate != nullptr && climate->weathers() != nullptr) {
        std::int32_t best = -1;
        for (const auto* entry : *climate->weathers()) {
            if (entry->chance() > best) {
                if (const auto* w = lookup(weathers, entry->weather())) {
                    weather = w;
                    best = entry->chance();
                }
            }
        }
    }
    const auto* colors = weather != nullptr ? weather->colors() : nullptr;
    if (colors == nullptr || colors->size() < 68) {
        return out;
    }

    // Times of day: sunrise, day, sunset, night. Keys at the start, middle and
    // end of sunrise and sunset; linear in between.
    float sun[4] = {5.5F, 10.0F, 16.0F, 20.5F};
    if (climate != nullptr) {
        sun[0] = climate->sunrise_begin();
        sun[1] = climate->sunrise_end();
        sun[2] = climate->sunset_begin();
        sun[3] = climate->sunset_end();
    }
    const float h = static_cast<float>(std::fmod(std::fmod(hour, 24.0) + 24.0, 24.0));
    struct Key {
        float hour;
        int time;
    };
    const Key keys[] = {{sun[0], 3},
                        {(sun[0] + sun[1]) / 2, 0},
                        {sun[1], 1},
                        {sun[2], 1},
                        {(sun[2] + sun[3]) / 2, 2},
                        {sun[3], 3}};
    int from = 3;
    int to = 3;
    float t = 0.0F;
    for (std::size_t i = 0; i + 1 < std::size(keys); ++i) {
        if (h >= keys[i].hour && h <= keys[i + 1].hour) {
            from = keys[i].time;
            to = keys[i + 1].time;
            const float span = keys[i + 1].hour - keys[i].hour;
            t = span > 0.0F ? (h - keys[i].hour) / span : 0.0F;
            break;
        }
    }
    const auto colour = [&](int index) {
        const Color a = unpack_color(colors->Get(static_cast<flatbuffers::uoffset_t>(index * 4 + from)));
        const Color b = unpack_color(colors->Get(static_cast<flatbuffers::uoffset_t>(index * 4 + to)));
        return a.lerp(b, t);
    };
    const auto weight = [](int time) { return time == 1 ? 1.0F : time == 3 ? 0.0F : 0.5F; };
    const float daylight = weight(from) + (weight(to) - weight(from)) * t;

    out["weather"] = to_godot(weather->editor_id());
    out["sky_upper"] = colour(0);
    out["fog_near_color"] = colour(1);
    out["ambient"] = colour(3);
    out["sunlight"] = colour(4);
    out["sky_lower"] = colour(7);
    out["horizon"] = colour(8);
    out["fog_far_color"] = colour(12);
    out["daylight"] = daylight;
    if (const auto* fog = weather->fog(); fog != nullptr && fog->size() >= 8) {
        const auto mix = [&](flatbuffers::uoffset_t day, flatbuffers::uoffset_t night) {
            return static_cast<double>(fog->Get(night) + (fog->Get(day) - fog->Get(night)) * daylight);
        };
        out["fog_near"] = mix(0, 2) * formats::k_metres_per_unit;
        out["fog_far"] = mix(1, 3) * formats::k_metres_per_unit;
        out["fog_power"] = mix(4, 5);
        out["fog_max"] = mix(6, 7);
    }

    // The sun rises in the east (+X), peaks in the south (+Z) and sets in the
    // west; between sunset and sunrise the moon takes its place.
    const float pi = std::numbers::pi_v<float>;
    const bool day = h >= sun[0] && h <= sun[3];
    float phase = 0.0F;
    if (day) {
        phase = (h - sun[0]) / std::max(sun[3] - sun[0], 0.1F);
    } else {
        const float night_length = 24.0F - (sun[3] - sun[0]);
        phase = std::fmod(h - sun[3] + 24.0F, 24.0F) / std::max(night_length, 0.1F);
    }
    const float azimuth = pi * phase; // 0 east, pi west
    const float elevation = std::sin(pi * phase) * pi * 0.38F + 0.05F;
    out["sun_direction"] = Vector3(std::cos(azimuth) * std::cos(elevation), std::sin(elevation),
                                   std::sin(azimuth) * std::cos(elevation))
                               .normalized();
    return out;
}

} // namespace skydot::queries
