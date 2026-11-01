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
    std::vector<std::string> traces;
    std::vector<Shader> shaders;
    std::vector<Message> messages;

    void trace(const std::string& text) override { traces.push_back(text); }
    void message(const std::string& text, bool box, bool help) override { messages.push_back({text, box, help}); }
    void effect_shader(std::uint32_t shader, std::uint32_t target, bool play) override {
        shaders.push_back({shader, target, play});
    }
};

} // namespace skydot::testing
