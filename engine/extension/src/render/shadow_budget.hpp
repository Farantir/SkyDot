// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotShadowBudget`: lets at most four of the placed lights below its parent
// cast shadows at a time, the ones nearest the camera. The game has the same
// limit: its lighting shader reads the shadows of placed lights from the four
// channels of one shadow mask (Lighting.hlsl: numShadowLights = min(4, ...),
// ShadowLightMaskSelect), so a cell full of shadow-flagged torches shows four
// shadowed lights and lights the rest without shadows. Bleak Falls Barrow has
// 76 shadowed lights in its first cell; drawing all of their shadow maps took
// 6200 of 8000 draw calls and half the frame (181647 camera: 21 ms -> 11 ms).
//
// Which four the game picks is not measured: nearest to the camera is assumed.
// The lights it manages are those in the group SkydotShadowBudget::GROUP
// (CellBuilder puts interior lights with a shadow flag there).
#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

namespace skydot {

class SkydotShadowBudget : public godot::Node {
    GDCLASS(SkydotShadowBudget, godot::Node)

public:
    /// Group of the lights that may cast shadows when the budget allows.
    static constexpr const char* GROUP = "skydot_shadow_lights";
    /// The game's limit of shadowed placed lights at once.
    static constexpr std::int64_t GAME_LIMIT = 4;

    /// How many lights may cast shadows (default GAME_LIMIT).
    void set_limit(std::int64_t limit) { limit_ = limit; }
    std::int64_t get_limit() const { return limit_; }

    /// Off: the budget leaves every light's shadow as it is (--light-shadows all).
    void set_active(bool active);
    bool is_active() const { return active_; }

    /// Gives shadows to the `limit` visible lights of the group below the parent
    /// nearest `position` (global), and takes them from the others. A light
    /// keeps its shadow until another is nearer by more than a metre, so two
    /// lights about as far do not take turns. Returns how many cast shadows.
    std::int64_t update_from(const godot::Vector3& position);

    void _ready() override;
    void _process(double delta) override;

protected:
    static void _bind_methods();

private:
    std::int64_t limit_ = GAME_LIMIT;
    bool active_ = true;
    double since_ = 1e9; // seconds since the last update
};

} // namespace skydot
