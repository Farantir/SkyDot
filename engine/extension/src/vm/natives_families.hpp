// SPDX-License-Identifier: GPL-3.0-or-later
//
// The natives families bound as free functions over narrow interfaces
// (ARCHITECTURE.md, "Natives and the world"). Each is in vm/natives_<family>.cpp.
#pragma once

#include "vm/actor_host.hpp"
#include "vm/vm.hpp"
#include "vm/quest_host.hpp"
#include "vm/reference_host.hpp"
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

/// Quest, Alias, ReferenceAlias, LocationAlias and GlobalVariable natives,
/// TopicInfo.GetOwningQuest and Game.GetFormFromFile.
void bind_quest_natives(Vm& vm, const WorldAccess& world, QuestHost& host);

/// Game.GetPlayer and GetForm.
void bind_game_natives(Vm& vm, QuestHost& quests);

/// ObjectReference natives: enable state, links, placement, animation,
/// physics, activation, doors and locks, inventory, triggers.
void bind_objectreference_natives(Vm& vm, const WorldAccess& world, WorldEvents& events, ReferenceHost& host);

/// Actor natives beyond values: factions, kill, relationships, base flags,
/// ghosts, locations, and the effects and animations the game layer shows;
/// also the reference's location and worldspace.
void bind_actor_extra_natives(Vm& vm, const WorldAccess& world, WorldEvents& events, ActorHost& host);

} // namespace skydot::vm
