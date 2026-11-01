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
    std::vector<std::string> traces;
    std::vector<Message> messages;

    void trace(const std::string& text) override { traces.push_back(text); }
    void message(const std::string& text, bool box, bool help) override { messages.push_back({text, box, help}); }
};

} // namespace skydot::testing
