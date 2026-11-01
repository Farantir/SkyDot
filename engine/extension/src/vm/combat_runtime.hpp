// SPDX-License-Identifier: GPL-3.0-or-later
//
// The combat domain of the Papyrus runtime: the hooks the combat AI answers the
// combat natives with, who hurt the player last, whether the player is dead,
// the ghosts already reported, and the combat natives. No Godot: it reaches the
// engine through ActorHost (actor states) and CombatRuntime::Host (the log).
#pragma once

#include "vm/actor_host.hpp"
#include "vm/vm.hpp"

#include <cstdint>
#include <functional>
#include <set>
#include <string>

namespace skydot::vm {

/// What the combat AI answers the combat natives with (set by SkydotAi, which owns the
/// state; cleared when it goes). Without them every actor is out of combat.
struct CombatHooks {
    std::function<std::int64_t(std::uint32_t ref)> state;    ///< 0 none, 1 combat, 2 searching
    std::function<std::uint32_t(std::uint32_t ref)> target;
    std::function<void(std::uint32_t ref, std::uint32_t target)> start;
    std::function<void(std::uint32_t ref)> stop;
    std::function<bool(std::uint32_t ref, std::uint32_t target)> has_los;  ///< Actor.HasLOS
    std::function<bool(std::uint32_t ref)> is_flying;          ///< Actor.IsFlying
    std::function<std::int64_t(std::uint32_t ref)> flying_state; ///< Actor.GetFlyingState (0..4)
    std::function<bool(std::uint32_t ref)> can_fly_here;       ///< Actor.CanFlyHere
    std::function<double(std::uint32_t ref)> length;           ///< ObjectReference.GetLength (units)
    /// The actor dies in a way that takes time (a death clip, a crash): OnDeath waits for finish_dying.
    std::function<bool(std::uint32_t ref)> defers_death;
    /// Actor.EvaluatePackage: choose the package again now.
    std::function<void(std::uint32_t ref)> evaluate_package;
};

class CombatRuntime {
public:
    class Host {
    public:
        virtual ~Host() = default;
        virtual void log(const std::string& text) = 0;
    };

    /// Bind the combat natives to `vm` and remember the hosts (SkydotPapyrus::bind_natives).
    void bind_natives(Vm& vm, ActorHost& actors, Host& host);

    void set_hooks(CombatHooks hooks) { hooks_ = std::move(hooks); }
    [[nodiscard]] const CombatHooks& hooks() const { return hooks_; }
    /// Whether `ref` is in combat or searching, as the AI says (false without hooks).
    [[nodiscard]] bool in_combat(std::uint32_t ref) const { return hooks_.state && hooks_.state(ref) != 0; }

    /// A ghost takes no hits: true (and says so once per actor) when `ref` is one.
    bool ghost_ignores_hit(std::uint32_t ref);
    void forget_ghost(std::uint32_t ref) { ghost_reported_.erase(ref); }
    [[nodiscard]] std::size_t ghost_count() const { return ghost_reported_.size(); }

    /// Who last hurt the player.
    [[nodiscard]] std::uint32_t last_damager() const { return last_damager_; }
    void set_last_damager(std::uint32_t who) { last_damager_ = who; }
    void forget_damager(std::uint32_t ref) { last_damager_ = last_damager_ == ref ? 0 : last_damager_; }
    [[nodiscard]] std::size_t damager_count() const { return last_damager_ != 0 ? 1 : 0; }

    [[nodiscard]] bool player_dead() const { return player_dead_; }
    /// The player's Health went up again (a restart) or a game was loaded: not dead, nobody to blame.
    void player_restored() { player_dead_ = false; last_damager_ = 0; }
    /// Look at the player's Health. The dead do not regenerate (a positive Health is damaged away). True when
    /// the player has just died (logged here); the caller announces it.
    bool check_player_death();

private:
    Vm* vm_ = nullptr;
    ActorHost* actors_ = nullptr;
    Host* host_ = nullptr;
    CombatHooks hooks_;
    std::set<std::uint32_t> ghost_reported_;
    std::uint32_t last_damager_ = 0;
    bool player_dead_ = false;
};

} // namespace skydot::vm
