// SPDX-License-Identifier: GPL-3.0-or-later
//
// Family hosts for natives tests: they hand out ids or record what a family
// asked, with the knobs a test needs.
#pragma once

#include "vm/quest_host.hpp"
#include "vm/sound_host.hpp"

#include <cstdint>
#include <vector>

namespace skydot::testing {

/// Playback ids 1, 2, ...; `exhausted` makes them run out.
struct FakeSounds final : skydot::vm::SoundHost {
    std::int64_t next = 1;
    bool exhausted = false;
    std::int64_t new_sound_instance() override { return exhausted ? 0 : next++; }
};

/// Records the quests the natives asked to have their scripts attached.
struct FakeQuests final : skydot::vm::QuestHost {
    struct Attach {
        std::uint32_t quest = 0;
        bool on_init = false;
    };
    std::vector<Attach> attached;
    void attach_quest_scripts(std::uint32_t quest, bool on_init) override { attached.push_back({quest, on_init}); }
};

} // namespace skydot::testing
