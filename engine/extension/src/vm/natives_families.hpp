// SPDX-License-Identifier: GPL-3.0-or-later
//
// The natives families bound as free functions over narrow interfaces
// (ARCHITECTURE.md, "Natives and the world"). Each is in vm/natives_<family>.cpp.
#pragma once

#include "vm/vm.hpp"
#include "vm/quest_host.hpp"
#include "vm/sound_host.hpp"
#include "vm/world_access.hpp"
#include "vm/world_events.hpp"

namespace skydot::vm {

/// Natives that are meant to do nothing: silent, or logged as not shown.
void bind_noop_natives(Vm& vm);

/// Utility.Wait, GetCurrentRealTime, RandomInt and RandomFloat.
void bind_utility_natives(Vm& vm);

/// Debug.Trace, Notification, MessageBox and Message.Show. `world` and
/// `events` must outlive the VM.
void bind_debug_natives(Vm& vm, const WorldAccess& world, WorldEvents& events);

/// EffectShader.Play and Stop.
void bind_effectshader_natives(Vm& vm, WorldEvents& events);

/// Weapon.Fire.
void bind_weapon_natives(Vm& vm, WorldEvents& events);

/// Sound.Play, PlayAndWait, StopInstance and SetInstanceVolume.
void bind_sound_natives(Vm& vm, SoundHost& sounds, WorldEvents& events);

/// Form.GetFormID, the update timers and the animation event registrations.
void bind_form_natives(Vm& vm, const WorldAccess& world, WorldEvents& events);

/// Game.GetPlayer and GetForm.
void bind_game_natives(Vm& vm, QuestHost& quests);

} // namespace skydot::vm
