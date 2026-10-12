// SPDX-License-Identifier: GPL-3.0-or-later
//
// Who may activate what: the rules `SkydotPapyrus::activate_ref` applies
// before anything runs. Nothing here touches Godot.
//
//   - A reference that answers only to its activate parents refuses the
//     player; a script's Activate and an activate parent reach it.
//   - A locked reference refuses the player unless it picks the lock (the
//     force flag); scripts and parents are not refused, and none of them
//     unlocks it. Only the player's pick does.
#pragma once

#include <string>

namespace skydot {

enum class ActivationSource { player = 0, script = 1, parent = 2 };

enum class ActivationGate { pass, parent_only, locked };

struct ActivationRequest {
    ActivationSource source{ActivationSource::player};
    bool parent_activate_only{};
    /// The lock level if locked, else -1.
    int lock_level{-1};
    /// The player picks the lock (Shift+F, or the viewer's pick-locks).
    bool force{};
};

[[nodiscard]] ActivationGate activation_gate(const ActivationRequest& request);
/// Whether the activation picks the lock open as it goes.
[[nodiscard]] bool activation_unlocks(const ActivationRequest& request);
/// XLOC's level as the game names it.
[[nodiscard]] std::string lock_level_name(int level);

} // namespace skydot
