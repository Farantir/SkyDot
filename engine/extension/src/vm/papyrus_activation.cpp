// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus: the Godot-bound side of activations. The rules, the sequence
// and the harvests are vm/activation_runtime.hpp's; this turns the outcome
// into a Dictionary for the game layer and reads the behaviour graph.
#include "vm/papyrus.hpp"

#include "actors/behavior_player.hpp"
#include "vm/papyrus_common.hpp"

#include <algorithm>

using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Variant;

namespace skydot {

namespace {

String godot_of(const std::string& s) { return String::utf8(s.c_str(), static_cast<int>(s.size())); }

} // namespace

Dictionary SkydotPapyrus::outcome_dictionary(const ActivationOutcome& outcome) const {
    Dictionary out;
    out["ref"] = static_cast<std::int64_t>(outcome.ref);
    out["kind"] = godot_of(outcome.kind);
    out["note"] = godot_of(outcome.note);
    out["scripts"] = outcome.scripts;
    if (outcome.label) {
        out["label"] = godot_of(*outcome.label);
    }
    if (outcome.lock_level) {
        out["lock_level"] = *outcome.lock_level;
    }
    if (!outcome.event.empty()) {
        out["event"] = godot_of(outcome.event);
    }
    if (outcome.deferred) {
        out["deferred"] = true;
    }
    if (outcome.door) {
        out["door"] = world_->get_door(outcome.ref);
    }
    if (outcome.kind == "harvest") {
        out["harvest_form"] = static_cast<std::int64_t>(outcome.harvest_form);
        out["harvest_count"] = outcome.harvest_count;
        out["harvest_name"] = godot_of(outcome.harvest_name);
        out["harvest_sound"] = static_cast<std::int64_t>(outcome.harvest_sound);
    }
    if (outcome.kind == "crafting") {
        out["bench_type"] = outcome.bench_type;
    }
    return out;
}

// The rules are in vm/activation.hpp; this runs them and what follows.
Dictionary SkydotPapyrus::activate_ref(std::int64_t ref, std::int64_t activator, bool default_only,
                                       std::int64_t source, bool force) {
    return outcome_dictionary(activation_.activate(
        static_cast<std::uint32_t>(ref), activator, default_only,
        static_cast<ActivationSource>(std::clamp<std::int64_t>(source, 0, 2)), force));
}

void SkydotPapyrus::complete_activation(std::int64_t ref) {
    if (const auto outcome = activation_.complete(static_cast<std::uint32_t>(ref))) {
        emit_signal("activation_outcome", ref, outcome_dictionary(*outcome));
    }
}

bool SkydotPapyrus::graph_raises_activation(std::uint32_t form) const {
    godot::Node* node = SkydotWorld::ref_node(form);
    if (node == nullptr) {
        return false;
    }
    const auto* graph = godot::Object::cast_to<SkydotBehavior>(node->find_child("SkydotBehavior", true, false));
    return graph != nullptr && graph->raises_event("ObjectActivated");
}

} // namespace skydot
