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
//   - The player's use of furniture with a behaviour graph that raises
//     ObjectActivated takes effect when the graph says so: the scripts and
//     activate children wait (PendingActivations). A second use meanwhile is
//     ignored.
//   - An actor that activates a loose item (an ALCH, AMMO, ARMO, BOOK, INGR,
//     KEYM, MISC, SLGM, SCRL or WEAP reference) picks it up, unless scripts
//     blocked the activation.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

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
/// Whether an activation of a reference of record `type` waits for the
/// graph's ObjectActivated: the player's use of furniture whose graph raises it.
[[nodiscard]] bool activation_waits_for_graph(std::string_view type, ActivationSource source, bool graph_raises);

/// Whether a reference of record `type` is a loose item an activator picks up.
[[nodiscard]] bool activation_picks_up(std::string_view type);

/// Activations waiting for their ObjectActivated, one per reference.
class PendingActivations {
public:
    struct Entry {
        std::int64_t activator{};
        bool default_only{};
    };
    /// False if `ref` is already pending (the use is ignored).
    bool begin(std::uint32_t ref, Entry entry);
    /// Take the entry of `ref`, if it waits.
    std::optional<Entry> take(std::uint32_t ref);
    [[nodiscard]] bool pending(std::uint32_t ref) const { return entries_.count(ref) != 0; }
    void clear() { entries_.clear(); }

private:
    std::map<std::uint32_t, Entry> entries_;
};

/// XLOC's level as the game names it.
[[nodiscard]] std::string lock_level_name(int level);

} // namespace skydot
