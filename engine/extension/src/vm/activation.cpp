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

bool activation_picks_up(std::string_view type) {
    for (const std::string_view item : {"ALCH", "AMMO", "ARMO", "BOOK", "INGR", "KEYM", "MISC", "SLGM", "SCRL", "WEAP"}) {
        if (type == item) {
            return true;
        }
    }
    return false;
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

bool activation_waits_for_graph(std::string_view type, ActivationSource source, bool graph_raises) {
    return type == "FURN" && source == ActivationSource::player && graph_raises;
}

bool PendingActivations::begin(std::uint32_t ref, Entry entry) {
    return entries_.emplace(ref, entry).second;
}

std::optional<PendingActivations::Entry> PendingActivations::take(std::uint32_t ref) {
    const auto found = entries_.find(ref);
    if (found == entries_.end()) {
        return std::nullopt;
    }
    const Entry entry = found->second;
    entries_.erase(found);
    return entry;
}

} // namespace skydot
