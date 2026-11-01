// SPDX-License-Identifier: GPL-3.0-or-later
//
// A WorldEvents that records what natives sent, for natives tests.
#pragma once

#include "vm/world_events.hpp"

#include <string>
#include <vector>

namespace skydot::testing {

struct FakeEvents final : skydot::vm::WorldEvents {
    struct Message {
        std::string text;
        bool box = false;
        bool help = false;
    };
    struct Shader {
        std::uint32_t shader = 0;
        std::uint32_t target = 0;
        bool play = false;
    };
    struct Shot {
        std::uint32_t weapon = 0;
        std::uint32_t source = 0;
        std::uint32_t ammo = 0;
    };
    std::vector<Shot> shots;
    std::vector<std::string> traces;
    std::vector<Shader> shaders;
    std::vector<Message> messages;

    void trace(const std::string& text) override { traces.push_back(text); }
    void message(const std::string& text, bool box, bool help) override { messages.push_back({text, box, help}); }
    void effect_shader(std::uint32_t shader, std::uint32_t target, bool play) override {
        shaders.push_back({shader, target, play});
    }
    void fire_weapon(std::uint32_t weapon, std::uint32_t source, std::uint32_t ammo) override {
        shots.push_back({weapon, source, ammo});
    }
};

} // namespace skydot::testing
