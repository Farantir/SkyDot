// SPDX-License-Identifier: GPL-3.0-or-later
//
// `WorldEvents`: the one-way notices natives send to the game layer (what
// were `emit_signal` calls on SkydotPapyrus). A native says what happened;
// who shows it, plays it or logs it is the adapter's business
// (vm/papyrus.cpp), and a test records them.
#pragma once

#include <cstdint>
#include <string>

namespace skydot::vm {

class WorldEvents {
public:
    virtual ~WorldEvents() = default;

    /// Debug.Trace: `text` goes to the log and out as the `trace` signal.
    virtual void trace(const std::string& text) = 0;
    /// A message for the player: `box` is a message box, otherwise a
    /// notification (the `message` signal). `help` only picks how the log
    /// names it (a help message is a notification).
    virtual void message(const std::string& text, bool box, bool help) = 0;
    /// EffectShader.Play (`play`) or Stop on `target` (the `effect_shader` signal).
    virtual void effect_shader(std::uint32_t shader, std::uint32_t target, bool play) = 0;
    /// Weapon.Fire: launch `ammo`'s projectile from `source` (either may be 0;
    /// the game layer decides what that means) (the `fire_weapon` signal).
    virtual void fire_weapon(std::uint32_t weapon, std::uint32_t source, std::uint32_t ammo) = 0;
    /// Play `sound` (a SNDR or SOUN) at `source` (0: not positional) as playback
    /// `instance` (the `play_sound` signal).
    virtual void play_sound(std::uint32_t sound, std::uint32_t source, std::int64_t instance) = 0;
    /// Stop playback `instance` (the `stop_sound` signal).
    virtual void stop_sound(std::int64_t instance) = 0;
    /// Set the volume of playback `instance` (the `sound_volume` signal).
    virtual void sound_volume(std::int64_t instance, double volume) = 0;
};

} // namespace skydot::vm
