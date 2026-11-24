// SPDX-License-Identifier: GPL-3.0-or-later
//
// The player's reference id, fixed by Skyrim.esm (PlayerRef). Nothing here
// touches Godot.
#pragma once

#include <cstdint>

namespace skydot {

/// The player's reference (its base is the NPC_ 0x7).
inline constexpr std::uint32_t k_player_ref = 0x14;

} // namespace skydot
