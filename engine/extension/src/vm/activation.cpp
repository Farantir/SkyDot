// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/activation.hpp"

namespace skydot {

ActivationGate activation_gate(const ActivationRequest& request) {
    if (request.source != ActivationSource::player) {
        return ActivationGate::pass;
    }
    if (request.parent_activate_only) {
        return ActivationGate::parent_only;
    }
    if (request.lock_level >= 0 && !request.force) {
        return ActivationGate::locked;
    }
    return ActivationGate::pass;
}

bool activation_unlocks(const ActivationRequest& request) {
    return request.source == ActivationSource::player && request.lock_level >= 0 && request.force;
}

std::string lock_level_name(int level) {
    if (level >= 255) {
        return "requires a key";
    }
    struct Step {
        int from;
        const char* name;
    };
    for (const Step& step : {Step{100, "Master"}, Step{75, "Expert"}, Step{50, "Adept"}, Step{25, "Apprentice"}}) {
        if (level >= step.from) {
            return step.name;
        }
    }
    return "Novice";
}

} // namespace skydot
