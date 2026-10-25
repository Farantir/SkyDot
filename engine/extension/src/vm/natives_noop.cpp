// SPDX-License-Identifier: GPL-3.0-or-later
//
// Natives that are meant to do nothing, because what they control does not
// exist in the engine and will not change what a script does next. They are
// silent and give the default of their return type; every other unbound
// native warns once (docs/papyrus.md).
#include "vm/papyrus.hpp"

#include <cstdio>

namespace skydot {

void SkydotPapyrus::bind_noop_natives() {
    // Event registrations for things nothing raises: nobody sleeps, no actor
    // has line of sight, no stats are tracked. The scripts wait for events
    // that never come, as they would in a world without them.
    for (const char* cls : {"Form", "Alias", "ActiveMagicEffect"}) {
        for (const char* fn : {"RegisterForSleep", "UnregisterForSleep", "RegisterForLOS", "UnregisterForLOS",
                               "RegisterForSingleLOSGain", "RegisterForSingleLOSLost",
                               "RegisterForTrackedStatsEvent", "UnregisterForTrackedStatsEvent"}) {
            vm_->ignore(cls, fn);
        }
    }
    // The story manager is not simulated: a story event starts no quest
    // (they return false).
    vm_->ignore("Keyword", "SendStoryEvent");
    vm_->ignore("Keyword", "SendStoryEventAndWait");
    // No blame or crime until there is combat.
    vm_->ignore("ObjectReference", "SetActorCause");
    // Feedback with no screen, controller or achievement system behind it.
    vm_->ignore("Game", "ShakeCamera");
    vm_->ignore("Game", "ShakeController");
    vm_->ignore("Game", "AddAchievement");
    vm_->ignore("Game", "IncrementStat");
    // Image space modifiers (a word wall's glow, a potion's tint) are screen effects that wait for the
    // renderer's post-processing: the call is logged, nothing is drawn.
    for (const char* fn : {"Apply", "ApplyCrossFade", "Remove", "PopTo"}) {
        vm_->bind("ImageSpaceModifier", fn, [fn](vm::NativeCall& c) {
            std::printf("papyrus: ImageSpaceModifier.%s 0x%X (no screen effect)\n", fn, c.self.form());
            return vm::NativeResult{};
        });
    }
    vm_->bind("ImageSpaceModifier", "RemoveCrossFade", [](vm::NativeCall&) {
        std::printf("papyrus: ImageSpaceModifier.RemoveCrossFade (no screen effect)\n");
        return vm::NativeResult{};
    });
}

} // namespace skydot
