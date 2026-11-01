// SPDX-License-Identifier: GPL-3.0-or-later
//
// `WorldEvents`: the one-way notices natives send to the game layer (what
// were `emit_signal` calls on SkydotPapyrus). A native says what happened;
// who shows it, plays it or logs it is the adapter's business
// (vm/papyrus.cpp), and a test records them.
#pragma once

#include "vm/world_access.hpp"

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
    /// A script registered for animation events of `sender`: the game layer
    /// should report them (the `animation_events_wanted` signal).
    virtual void animation_events_wanted(std::uint32_t sender) = 0;
    /// `ref` was moved or reset to this place (the `ref_moved` signal).
    virtual void ref_moved(std::uint32_t ref, const Xyz& position, const Xyz& rotation) = 0;
    /// Play the named animation on `ref` (the `play_animation` signal).
    virtual void play_animation(std::uint32_t ref, const std::string& animation) = 0;
    /// Push `ref`'s body along `direction` (Skyrim's space) with Havok's `magnitude` (the `havok_impulse` signal).
    virtual void havok_impulse(std::uint32_t ref, const Xyz& direction, double magnitude) = 0;
    /// `source` knocks `target` over with `force` (the `push_actor` signal).
    virtual void push_actor(std::uint32_t source, std::uint32_t target, double force) = 0;
    /// Everything within `radius` of `source` is knocked away (the `knock_area` signal).
    virtual void knock_area(std::uint32_t source, double magnitude, double radius) = 0;
    /// `ref`'s motion type was set (the `motion_type_changed` signal).
    virtual void motion_type_changed(std::uint32_t ref, std::int32_t motion_type) = 0;
    /// A script opened or closed `ref` (the `open_changed` signal).
    virtual void open_changed(std::uint32_t ref, bool open) = 0;
    /// A visual effect starts (`play`, with `art` and `duration`; `target` 0 for none) or stops on `ref`
    /// (the `visual_effect` signal).
    virtual void visual_effect(std::uint32_t ref, std::uint32_t art, std::uint32_t target, double duration,
                               bool play) = 0;
    /// Play the named sub graph animation on `ref` (the `sub_graph_animation` signal).
    virtual void sub_graph_animation(std::uint32_t ref, const std::string& animation) = 0;
};

} // namespace skydot::vm
