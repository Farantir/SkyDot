// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/furniture.hpp"

#include "ai/idle_choice.hpp"
#include "render/furniture_markers.hpp"
#include "vm/condition_world.hpp"
#include "world/world.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>

namespace skydot {
namespace {

using godot::Dictionary;
using godot::String;
using godot::Variant;

std::string utf8(const String& s) { return s.utf8().get_data(); }

/// The game as far as the pack knows it, plus the furniture facts a caller states.
class Facts final : public PapyrusConditionWorld {
public:
    Facts(const SkydotWorld* world, const Dictionary& facts, std::uint32_t actor, std::uint32_t furniture)
        : PapyrusConditionWorld(nullptr, world), facts_(facts), actor_(actor), furniture_(furniture) {}

    /// The actor is linked to its furniture (the ambush spider's link in the editor).
    std::uint32_t linked_ref(std::uint32_t ref, std::uint32_t keyword) const override {
        return ref == actor_ && keyword == 0 ? furniture_ : 0;
    }

    std::int32_t sitting_state(std::uint32_t) const override { return get("sitting"); }
    std::int32_t sleeping_state(std::uint32_t) const override { return get("sleeping"); }
    std::int32_t furniture_anim_type(std::uint32_t) const override { return get("anim_type"); }
    bool entering_interaction_quick(std::uint32_t) const override { return get("quick") != 0; }
    std::int32_t furniture_entry_type(std::uint32_t) const override { return get("entry"); }
    bool exiting_instant(std::uint32_t) const override { return get("exit_instant") != 0; }
    bool exiting_interaction_quick(std::uint32_t) const override { return get("exit_quick") != 0; }
    std::optional<std::int32_t> graph_variable_int(std::uint32_t, const std::string& name) const override {
        const Variant vars = facts_.get("vars", Variant());
        if (vars.get_type() != Variant::DICTIONARY) {
            return std::nullopt;
        }
        const Dictionary d = vars;
        const String key = String::utf8(name.c_str());
        return d.has(key) ? std::optional<std::int32_t>(static_cast<std::int32_t>(static_cast<std::int64_t>(d[key])))
                          : std::nullopt;
    }

private:
    std::int32_t get(const char* key) const {
        const Variant v = facts_.get(key, 0);
        return v.get_type() == Variant::BOOL ? (static_cast<bool>(v) ? 1 : 0)
                                             : static_cast<std::int32_t>(static_cast<std::int64_t>(v));
    }

    Dictionary facts_;
    std::uint32_t actor_;
    std::uint32_t furniture_;
};

Dictionary motion_dictionary(const std::optional<RootMotion>& m) {
    Dictionary d;
    if (m) {
        d["x"] = static_cast<double>(m->x);
        d["y"] = static_cast<double>(m->y);
        d["z"] = static_cast<double>(m->z);
        d["yaw"] = static_cast<double>(m->yaw);
    }
    return d;
}

} // namespace

void SkydotFurniture::_bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("setup", "world"), &SkydotFurniture::setup);
    godot::ClassDB::bind_method(godot::D_METHOD("choose_idle", "action", "behaviour", "actor", "furniture", "facts"),
                                &SkydotFurniture::choose_idle);
    godot::ClassDB::bind_method(godot::D_METHOD("open_graph", "behaviour"), &SkydotFurniture::open_graph);
    godot::ClassDB::bind_method(godot::D_METHOD("get_missing"), &SkydotFurniture::get_missing);
    godot::ClassDB::bind_method(godot::D_METHOD("get_graph_behaviour"), &SkydotFurniture::get_graph_behaviour);
    godot::ClassDB::bind_method(godot::D_METHOD("get_markers", "model"), &SkydotFurniture::get_markers);
    godot::ClassDB::bind_method(godot::D_METHOD("graph_start"), &SkydotFurniture::graph_start);
    godot::ClassDB::bind_method(godot::D_METHOD("graph_send", "event"), &SkydotFurniture::graph_send);
    godot::ClassDB::bind_method(godot::D_METHOD("graph_enter", "event"), &SkydotFurniture::graph_enter);
    godot::ClassDB::bind_method(godot::D_METHOD("graph_in_furniture"), &SkydotFurniture::graph_in_furniture);
    godot::ClassDB::bind_method(godot::D_METHOD("graph_advance", "seconds"), &SkydotFurniture::graph_advance);
    godot::ClassDB::bind_method(godot::D_METHOD("graph_playing"), &SkydotFurniture::graph_playing);
    godot::ClassDB::bind_method(godot::D_METHOD("graph_root_motion"), &SkydotFurniture::graph_root_motion);
    godot::ClassDB::bind_method(godot::D_METHOD("root_motion", "clip", "time"), &SkydotFurniture::root_motion);
}

godot::Error SkydotFurniture::setup(const godot::Ref<SkydotWorld>& world) {
    world_ = world;
    return world_.is_valid() ? godot::OK : godot::ERR_UNCONFIGURED;
}

Dictionary SkydotFurniture::choose_idle(std::int64_t action, const String& behaviour, std::int64_t actor,
                                        std::int64_t furniture, const Dictionary& facts) const {
    Dictionary out;
    if (!world_.is_valid()) {
        return out;
    }
    Facts world(world_.ptr(), facts, static_cast<std::uint32_t>(actor), static_cast<std::uint32_t>(furniture));
    conditions::ConditionContext context;
    context.subject = static_cast<std::uint32_t>(actor);
    context.target = static_cast<std::uint32_t>(furniture);
    context.unknown = conditions::UnknownPolicy::zero;
    const auto choice = ai::choose_idle(world_->data(), static_cast<std::uint32_t>(action), utf8(behaviour), world, context);
    if (choice) {
        out["idle"] = static_cast<std::int64_t>(choice->idle);
        out["event"] = String::utf8(choice->event.c_str());
        if (const auto* record = world_->data().idle_ptr(choice->idle); record != nullptr && record->editor_id() != nullptr) {
            out["editor_id"] = String::utf8(record->editor_id()->c_str());
        }
    }
    godot::Array unknown;
    for (const auto& [function, count] : world.unknowns().counts()) {
        unknown.push_back(static_cast<std::int64_t>(function));
    }
    out["unknown"] = unknown;
    return out;
}

bool SkydotFurniture::open_graph(const String& behaviour) {
    graph_.reset();
    missing_ = "";
    if (!world_.is_valid()) {
        missing_ = "no world";
        return false;
    }
    ActorGraphLibrary& library = world_->graph_library();
    const auto def = library.graph(utf8(behaviour));
    if (def->empty()) {
        missing_ = String::utf8(def->missing.c_str());
        return false;
    }
    graph_ = std::make_unique<ActorGraph>(def, [&library](const std::string& file) { return library.clip_duration(file); });
    graph_->configure(false);
    return true;
}

String SkydotFurniture::get_graph_behaviour() const {
    return graph_ ? String::utf8(graph_->def().behaviour.c_str()) : String();
}

godot::Array SkydotFurniture::get_markers(godot::Node* model) const {
    godot::Array out;
    for (const FurnitureMarker& m : furniture_markers(model)) {
        Dictionary d;
        d["offset"] = godot::Vector3(m.offset[0], m.offset[1], m.offset[2]);
        d["heading"] = static_cast<double>(m.heading);
        d["animation_type"] = m.animation_type;
        d["entry_points"] = m.entry_points;
        out.push_back(d);
    }
    return out;
}

Dictionary SkydotFurniture::report(const BehaviorOutput& out) const {
    Dictionary d;
    godot::Array events;
    for (const std::string& e : out.events) {
        events.push_back(String::utf8(e.c_str()));
    }
    godot::Array changes;
    for (const BehaviorChange& c : out.changes) {
        changes.push_back(String::utf8((std::to_string(c.machine) + ": " + c.from + " -> " + c.to +
                                        (c.event.empty() ? "" : " (" + c.event + ")")).c_str()));
    }
    d["events"] = events;
    d["changes"] = changes;
    d["accepted"] = out.accepted;
    d["playing"] = graph_playing();
    return d;
}

Dictionary SkydotFurniture::graph_start() { return graph_ ? report(graph_->start()) : Dictionary(); }
Dictionary SkydotFurniture::graph_send(const String& event) { return graph_ ? report(graph_->send(utf8(event))) : Dictionary(); }
Dictionary SkydotFurniture::graph_enter(const String& event) {
    return graph_ ? report(graph_->send_entering(utf8(event))) : Dictionary();
}
Dictionary SkydotFurniture::graph_advance(double seconds) {
    return graph_ ? report(graph_->advance(static_cast<float>(seconds))) : Dictionary();
}

Dictionary SkydotFurniture::graph_playing() const {
    Dictionary d;
    if (graph_) {
        if (const auto now = graph_->playing()) {
            d["clip"] = String::utf8(now->clip.c_str());
            d["file"] = String::utf8(now->file.c_str());
            d["time"] = static_cast<double>(now->time);
            d["mode"] = now->mode;
            d["speed"] = static_cast<double>(now->speed);
        }
    }
    return d;
}

Dictionary SkydotFurniture::graph_root_motion() const {
    return graph_ ? motion_dictionary(graph_->root_motion()) : Dictionary();
}

Dictionary SkydotFurniture::root_motion(const String& clip, double time) const {
    return graph_ ? motion_dictionary(graph_->def().root_motion(utf8(clip), static_cast<float>(time))) : Dictionary();
}

} // namespace skydot
