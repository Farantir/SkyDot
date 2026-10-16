// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/activation.hpp"

#include <algorithm>
#include <cstdio>

namespace skydot {

namespace {

std::string label_of(std::uint32_t ref, const ActivationTarget& t) {
    char id[16];
    std::snprintf(id, sizeof id, "0x%08X", ref);
    return std::string(id) + " " + t.editor_id + " (" + t.type + ")";
}

} // namespace

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

ActivationOutcome Activations::activate(std::uint32_t ref, std::int64_t activator, bool default_only,
                                        ActivationSource source, bool force) {
    ActivationOutcome out;
    out.ref = ref;
    ActivationTarget target;
    if (!host_.target(ref, target)) {
        return out;
    }
    const std::string label = label_of(ref, target);
    out.label = label;
    const std::int64_t who = activator != 0 ? activator : static_cast<std::int64_t>(k_player_ref);
    ActivationRequest request;
    request.source = source;
    request.parent_activate_only = target.parent_activate_only;
    request.lock_level = host_.lock_level(ref);
    request.force = force;
    switch (activation_gate(request)) {
    case ActivationGate::parent_only:
        out.kind = "parent_only";
        out.note = label + " only responds to its activate parents";
        return out;
    case ActivationGate::locked:
        out.kind = "locked";
        out.lock_level = request.lock_level;
        out.note = label + " is locked (level " + std::to_string(request.lock_level) + ", " +
                   lock_level_name(request.lock_level) + ")";
        return out;
    case ActivationGate::pass:
        break;
    }
    if (activation_unlocks(request)) {
        host_.unlock(ref);
    }
    // A lever with a behaviour graph activates when the graph says so: the
    // scripts and activate children wait for its ObjectActivated.
    if (activation_waits_for_graph(target.type, request.source, host_.graph_raises_activation(ref))) {
        if (!pending_.begin(ref, {who, default_only})) {
            out.kind = "busy";
            out.note = label + " is still being used";
            return out;
        }
        out.kind = "furniture";
        out.event = "LeverPushStart";
        out.deferred = true;
        out.note = label + ": nothing happens";
        return out;
    }
    if (!run(ref, who, default_only, out)) {
        out.note = label + ": activation blocked, only its scripts ran";
        return out;
    }
    out.kind = "none";
    if (activation_picks_up(target.type)) {
        // Loose items go into the activator's inventory.
        target.count = std::max<std::int64_t>(target.count, 1);
        host_.pick_up(ref, who, target);
        out.kind = "pickup";
        out.note = label + ": picked up";
        return out;
    }
    if (target.load_door) {
        out.kind = "door";
        out.door = true;
    } else if (target.type == "FURN") {
        // A lever is furniture: using it pushes it (the player's push
        // animation, which sends Lever_PlayerPush in the game, does not exist).
        out.kind = "furniture";
        out.event = "LeverPushStart";
    } else if (target.type == "DOOR") {
        out.kind = "toggle_open";
    }
    out.note = label + ": nothing happens"; // if the host can do nothing with `kind`
    return out;
}

std::optional<ActivationOutcome> Activations::complete(std::uint32_t ref) {
    const auto taken = pending_.take(ref);
    if (!taken) {
        return std::nullopt;
    }
    ActivationOutcome out;
    out.ref = ref;
    out.kind = "none";
    ActivationTarget target;
    host_.target(ref, target); // an unknown reference labels as "0x... ()"
    out.label = label_of(ref, target);
    if (!run(ref, taken->activator, taken->default_only, out)) {
        out.note = *out.label + ": activation blocked, only its scripts ran";
    }
    return out;
}

bool Activations::run(std::uint32_t ref, std::int64_t activator, bool default_only, ActivationOutcome& out) {
    if (!default_only) {
        out.scripts = host_.send_activate(ref, activator);
    }
    // A script that blocks activation handles it alone, and its activate
    // children stay quiet.
    if (host_.activation_blocked(ref)) {
        out.kind = "blocked";
        return false;
    }
    // Activate children run on the VM's clock, activated by the parent: trap
    // scripts read the parent's trigger type from their activator (BladeTrap
    // casts it to TrapTriggerBase; a pull chain's type 2 stops the blades).
    for (const auto& child : host_.activate_children(ref)) {
        host_.schedule_child(child.delay, child.ref, ref);
    }
    return true;
}

} // namespace skydot
