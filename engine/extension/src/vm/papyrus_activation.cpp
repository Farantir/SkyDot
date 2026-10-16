// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus: the binding of activations. The rules and the sequence are
// vm/activation.hpp's (`Activations`); this answers what they ask of the
// world and the VM, and turns the outcome into a Dictionary for the game
// layer.
#include "vm/papyrus.hpp"

#include "actors/behavior_player.hpp"
#include "build/ref_nodes.hpp"
#include "vm/natives_common.hpp"

#include <algorithm>

using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Variant;

namespace skydot {

namespace {

String godot_of(const std::string& s) { return String::utf8(s.c_str(), static_cast<int>(s.size())); }

} // namespace

struct SkydotPapyrus::ActivationAdapter final : ActivationHost {
    explicit ActivationAdapter(SkydotPapyrus& owner) : owner_(owner) {}

    bool target(std::uint32_t ref, ActivationTarget& out) const override {
        const Dictionary info = owner_.ref_info(ref);
        if (info.is_empty()) {
            return false;
        }
        out.base = form_of(info.get("base", 0));
        out.type = natives::utf8(String(info.get("type", String())));
        out.editor_id = natives::utf8(String(info.get("editor_id", String())));
        out.parent_activate_only = static_cast<bool>(info.get("parent_activate_only", false));
        out.load_door = info.get("door", Variant()).get_type() == Variant::DICTIONARY;
        // XCNT is not in the reference info yet: one of them.
        out.count = static_cast<std::int64_t>(info.get("count", 1));
        return true;
    }
    int lock_level(std::uint32_t ref) const override { return static_cast<int>(owner_.get_lock_level(ref)); }
    void unlock(std::uint32_t ref) override { owner_.set_locked(ref, false); }
    bool graph_raises_activation(std::uint32_t ref) const override { return owner_.graph_raises_activation(ref); }
    std::int64_t send_activate(std::uint32_t ref, std::int64_t activator) override {
        return owner_.activate(ref, activator);
    }
    bool activation_blocked(std::uint32_t ref) const override { return owner_.is_activation_blocked(ref); }
    std::vector<Child> activate_children(std::uint32_t ref) const override {
        std::vector<Child> out;
        const Array children = owner_.world_->get_activate_children(ref);
        for (std::int64_t i = 0; i < children.size(); ++i) {
            const Dictionary child = children[i];
            out.push_back({form_of(child["ref"]), static_cast<double>(child.get("delay", 0.0))});
        }
        return out;
    }
    void schedule_child(double delay, std::uint32_t child, std::uint32_t parent) override {
        owner_.vm_->schedule(delay, "activate", child, parent, static_cast<std::uint32_t>(ActivationSource::parent));
    }
    void pick_up(std::uint32_t ref, std::int64_t activator, const ActivationTarget& item) override {
        owner_.vm_->inventory().add(static_cast<std::uint32_t>(activator), item.base, item.count);
        owner_.set_disabled(ref, true);
    }

private:
    SkydotPapyrus& owner_;
};

void SkydotPapyrus::bind_activations() {
    activation_host_ = std::make_unique<ActivationAdapter>(*this);
    activations_ = std::make_unique<Activations>(*activation_host_);
}

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
    return out;
}

// The rules are in vm/activation.hpp; this runs them and what follows.
Dictionary SkydotPapyrus::activate_ref(std::int64_t ref, std::int64_t activator, bool default_only,
                                       std::int64_t source, bool force) {
    return outcome_dictionary(activations_->activate(
        static_cast<std::uint32_t>(ref), activator, default_only,
        static_cast<ActivationSource>(std::clamp<std::int64_t>(source, 0, 2)), force));
}

void SkydotPapyrus::complete_activation(std::int64_t ref) {
    if (const auto outcome = activations_->complete(static_cast<std::uint32_t>(ref))) {
        emit_signal("activation_outcome", ref, outcome_dictionary(*outcome));
    }
}

bool SkydotPapyrus::graph_raises_activation(std::uint32_t form) const {
    godot::Node* node = ref_nodes::find(form);
    if (node == nullptr) {
        return false;
    }
    const auto* graph = godot::Object::cast_to<SkydotBehavior>(node->find_child("SkydotBehavior", true, false));
    return graph != nullptr && graph->raises_event("ObjectActivated");
}

} // namespace skydot
