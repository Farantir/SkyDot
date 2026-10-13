// SPDX-License-Identifier: GPL-3.0-or-later
//
// Natives that are meant to do nothing, because what they control does not
// exist in the engine and will not change what a script does next. They are
// silent and give the default of their return type; every other unbound
// native warns once (docs/papyrus.md).
#include "vm/papyrus.hpp"

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
}

} // namespace skydot
