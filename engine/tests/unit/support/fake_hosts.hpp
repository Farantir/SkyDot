// SPDX-License-Identifier: GPL-3.0-or-later
//
// Family hosts for natives tests: they hand out ids or record what a family
// asked, with the knobs a test needs.
#pragma once

#include "vm/sound_host.hpp"

#include <cstdint>

namespace skydot::testing {

/// Playback ids 1, 2, ...; `exhausted` makes them run out.
struct FakeSounds final : skydot::vm::SoundHost {
    std::int64_t next = 1;
    bool exhausted = false;
    std::int64_t new_sound_instance() override { return exhausted ? 0 : next++; }
};

} // namespace skydot::testing
