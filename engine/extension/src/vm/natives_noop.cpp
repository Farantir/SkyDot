// SPDX-License-Identifier: GPL-3.0-or-later
//
// Natives that are meant to do nothing, because what they control does not
// exist in the engine and will not change what a script does next. They are
// silent and give the default of their return type; every other unbound
// native warns once (docs/papyrus.md).
#include "vm/natives_families.hpp"

#include <cstdio>
#include <utility>

namespace skydot::vm {

void bind_noop_natives(Vm& vm) {
    // Event registrations for things nothing raises: no actor has line of
    // sight, no stats are tracked. The scripts wait for events that never
    // come, as they would in a world without them. (Sleep: natives_form.cpp.)
    for (const char* cls : {"Form", "Alias", "ActiveMagicEffect"}) {
        for (const char* fn : {"RegisterForLOS", "UnregisterForLOS",
                               "RegisterForSingleLOSGain", "RegisterForSingleLOSLost",
                               "RegisterForTrackedStatsEvent", "UnregisterForTrackedStatsEvent"}) {
            vm.ignore(cls, fn);
        }
    }
    // The story manager is not simulated: a story event starts no quest
    // (they return false).
    vm.ignore("Keyword", "SendStoryEvent");
    vm.ignore("Keyword", "SendStoryEventAndWait");
    // No blame or crime until there is combat.
    vm.ignore("ObjectReference", "SetActorCause");
    // Impact effects at a node of a model (a dragon's footfall dust, its tail stomp) and a named idle animation
    // (an actor plays the animation its package names; the script's idle is not looked up): logged, nothing
    // shown. (AddToMap: natives_map.cpp.)
    for (const auto& [cls, fn] : {std::pair{"ObjectReference", "PlayImpactEffect"}, std::pair{"Actor", "PlayIdle"}}) {
        vm.bind(cls, fn, [cls, fn](NativeCall& c) {
            char line[128];
            std::snprintf(line, sizeof line, "papyrus: %s.%s 0x%X (not shown)", cls, fn, c.self.form());
            c.vm.note(line);
            return NativeResult{};
        });
    }
    // Feedback with no screen, controller or achievement system behind it.
    vm.ignore("Game", "ShakeCamera");
    vm.ignore("Game", "ShakeController");
    vm.ignore("Game", "AddAchievement");
    // Image space modifiers (a word wall's glow, a potion's tint) are screen effects that wait for the
    // renderer's post-processing: the call is logged, nothing is drawn.
    for (const char* fn : {"Apply", "ApplyCrossFade", "Remove", "PopTo"}) {
        vm.bind("ImageSpaceModifier", fn, [fn](NativeCall& c) {
            char line[128];
            std::snprintf(line, sizeof line, "papyrus: ImageSpaceModifier.%s 0x%X (no screen effect)", fn,
                          c.self.form());
            c.vm.note(line);
            return NativeResult{};
        });
    }
    vm.bind("ImageSpaceModifier", "RemoveCrossFade", [](NativeCall& c) {
        c.vm.note("papyrus: ImageSpaceModifier.RemoveCrossFade (no screen effect)");
        return NativeResult{};
    });
}

} // namespace skydot::vm
