// SPDX-License-Identifier: GPL-3.0-or-later
#include "actors/behavior_player.hpp"

#include "actors/actor_animation.hpp"
#include "actors/locomotion.hpp"
#include "animation_generated.h"
#include "assets/pack.hpp"
#include "assets/vpath.hpp"
#include "render/animator.hpp"
#include "render/effect_asset.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace skydot {
namespace {

namespace afb = bethconv::pack::afb;
using godot::String;

constexpr std::uint32_t k_animation_format = 2;

const afb::Animation* read_asset(const godot::PackedByteArray& bytes) {
    const auto* data = bytes.ptr();
    if (data == nullptr || bytes.is_empty()) {
        return nullptr;
    }
    flatbuffers::Verifier verifier(data, static_cast<std::size_t>(bytes.size()));
    if (!afb::VerifyAnimationBuffer(verifier)) {
        return nullptr;
    }
    const auto* root = afb::GetAnimation(data);
    return root->format_version() == k_animation_format ? root : nullptr;
}

/// The converter's `behavior_graph` extra, on the model's root block.
std::string behavior_graph_path(godot::Node* node, int depth = 0) {
    const godot::Dictionary block = bethconv_extras(node);
    if (block.has("behavior_graph")) {
        return std::string(String(block["behavior_graph"]).utf8().get_data());
    }
    if (depth < 3) {
        for (int i = 0; i < node->get_child_count(); ++i) {
            std::string path = behavior_graph_path(node->get_child(i), depth + 1);
            if (!path.empty()) {
                return path;
            }
        }
    }
    return {};
}

void collect_nodes(godot::Node* node, std::unordered_map<std::string, godot::Node3D*>& out) {
    if (auto* n3 = godot::Object::cast_to<godot::Node3D>(node)) {
        out.emplace(ascii_lower(std::string(String(node->get_name()).utf8().get_data())), n3);
    }
    for (int i = 0; i < node->get_child_count(); ++i) {
        collect_nodes(node->get_child(i), out);
    }
}

/// Whether `key` has not been reported yet: a diagnostic per project, not per
/// placed model (a cell can hold hundreds).
bool first_report(const std::string& key) {
    static std::mutex mutex;
    static std::unordered_set<std::string> seen;
    const std::lock_guard lock(mutex);
    return seen.insert(key).second;
}

} // namespace

godot::Node* SkydotBehavior::attach(godot::Node* model, godot::Object* pack_object, std::int64_t ref) {
    auto* pack = godot::Object::cast_to<SkydotPack>(pack_object);
    if (pack == nullptr) {
        return nullptr;
    }
    return attach_from(model, [pack](const std::string& vpath) { return pack->get_bytes(String::utf8(vpath.c_str())); },
                       ref);
}

SkydotBehavior* SkydotBehavior::attach_from(godot::Node* model, const BytesFn& bytes, std::int64_t ref) {
    if (model == nullptr || model->find_child("SkydotBehavior", true, false) != nullptr) {
        return nullptr;
    }
    const std::string graph_file = behavior_graph_path(model);
    if (graph_file.empty()) {
        return nullptr;
    }
    // meshes/<folder>/<project>.hkx
    const std::string path = "meshes/" + normalize_path(graph_file);
    const auto slash = path.rfind('/');
    const auto dot = path.rfind('.');
    if (slash == std::string::npos || dot == std::string::npos || dot < slash) {
        return nullptr;
    }
    const std::string folder = path.substr(0, slash + 1);
    const std::string project = path.substr(slash + 1, dot - slash - 1);

    // The character: the project file's "Characters\..." entry. Each way to
    // miss it is its own message.
    const auto say = [&](const char* what) {
        if (!first_report(project + what)) {
            return;
        }
        godot::UtilityFunctions::print(String("0x%08X behaviour: %s (%s)") %
                                       godot::Array::make(ref, String::utf8(what), String::utf8(project.c_str())));
    };
    std::string rig_file;
    std::string behavior_file;
    bool project_listed = false;
    bool character_read = false;
    const godot::PackedByteArray project_bytes = bytes("meshes/animationdata/" + project + ".txt");
    const auto* proj = read_asset(project_bytes);
    if (proj == nullptr || proj->project_files() == nullptr) {
        say("project file animationdata/<name>.txt is missing from the pack or has no file list");
        return nullptr;
    }
    for (const auto* f : *proj->project_files()) {
        const std::string file = normalize_path(f->str());
        if (!file.starts_with("characters/")) {
            continue;
        }
        project_listed = true;
        const godot::PackedByteArray character_bytes = bytes(folder + file);
        const auto* character = read_asset(character_bytes);
        if (character != nullptr && character->characters() != nullptr && character->characters()->size() > 0) {
            character_read = true;
            const auto* c = character->characters()->Get(0);
            rig_file = c->rig() != nullptr ? normalize_path(c->rig()->str()) : std::string();
            behavior_file = c->behavior() != nullptr ? normalize_path(c->behavior()->str()) : std::string();
            break;
        }
    }
    if (!project_listed) {
        say("the project lists no character file");
        return nullptr;
    }
    if (!character_read) {
        say("the character file is missing from the pack or unreadable");
        return nullptr;
    }
    if (rig_file.empty() || behavior_file.empty()) {
        say("the character names no rig or no behaviour file");
        return nullptr;
    }
    const godot::PackedByteArray skeleton_bytes = bytes(folder + rig_file);
    const godot::PackedByteArray behavior_bytes = bytes(folder + behavior_file);
    const auto* behavior_asset = read_asset(behavior_bytes);
    if (skeleton_bytes.is_empty() || behavior_asset == nullptr) {
        if (first_report(project + "|assets")) {
            godot::UtilityFunctions::print(String("0x%08X behaviour: no skeleton or graph in the pack for %s") %
                                       godot::Array::make(ref, String::utf8(project.c_str())));
        }
        return nullptr;
    }
    BehaviorDef def = behavior_from_asset(*behavior_asset);
    if (def.states.empty()) {
        return nullptr;
    }

    std::unordered_map<std::string, godot::Node3D*> nodes;
    collect_nodes(model, nodes);

    auto* player = memnew(SkydotBehavior);
    player->set_name("SkydotBehavior");
    player->ref_ = ref;
    // One animation per distinct file, shared by the clips that name it.
    std::unordered_map<std::string, std::size_t> by_file;
    for (BehaviorClip& clip : def.clips) {
        ClipPose pose;
        const std::string file = normalize_path(folder + clip.animation);
        if (const auto it = by_file.find(file); it != by_file.end()) {
            pose = player->poses_[it->second];
        } else {
            const godot::PackedByteArray clip_bytes = bytes(file);
            if (!clip_bytes.is_empty()) {
                pose.animation = SkydotAnimation::build_clip_for(clip_bytes, skeleton_bytes, "b");
            }
            if (pose.animation.is_valid()) {
                player->bind_tracks(pose, nodes);
            } else {
                godot::UtilityFunctions::print(
                    String("0x%08X behaviour: clip %s has no animation %s") %
                    godot::Array::make(ref, String::utf8(clip.name.c_str()), String::utf8(file.c_str())));
            }
            by_file.emplace(file, player->poses_.size());
        }
        clip.duration = pose.animation.is_valid() ? static_cast<float>(pose.animation->get_length()) : 0.0F;
        player->poses_.push_back(std::move(pose));
    }
    player->finish(model, std::move(def));
    return player;
}

SkydotBehavior* SkydotBehavior::attach_graph(godot::Node* model, const godot::Dictionary& graph, std::int64_t ref) {
    if (model == nullptr || model->find_child("SkydotBehavior", true, false) != nullptr) {
        return nullptr;
    }
    std::unordered_map<std::string, godot::Node3D*> nodes;
    collect_nodes(model, nodes);
    auto* player = memnew(SkydotBehavior);
    player->set_name("SkydotBehavior");
    player->ref_ = ref;
    BehaviorDef def;
    def.start_state = static_cast<int>(graph.get("start", 0));
    for (const godot::Variant& c : godot::Array(graph.get("clips", godot::Array()))) {
        const godot::Dictionary clip = c;
        ClipPose pose;
        pose.animation = clip.get("animation", godot::Variant());
        player->bind_tracks(pose, nodes);
        BehaviorClip out;
        out.name = std::string(String(clip.get("name", "")).utf8().get_data());
        out.mode = static_cast<int>(clip.get("mode", 0));
        out.duration = pose.animation.is_valid() ? static_cast<float>(pose.animation->get_length()) : 0.0F;
        for (const godot::Variant& t : godot::Array(clip.get("triggers", godot::Array()))) {
            const godot::Array trigger = t;
            out.triggers.push_back({static_cast<float>(static_cast<double>(trigger[0])),
                                    std::string(String(trigger[1]).utf8().get_data()), false});
        }
        def.clips.push_back(std::move(out));
        player->poses_.push_back(std::move(pose));
    }
    for (const godot::Variant& st : godot::Array(graph.get("states", godot::Array()))) {
        const godot::Dictionary state = st;
        BehaviorState out;
        out.id = static_cast<int>(state.get("id", 0));
        out.name = std::string(String(state.get("name", "")).utf8().get_data());
        out.clip = static_cast<int>(state.get("clip", -1));
        for (const godot::Variant& t : godot::Array(state.get("transitions", godot::Array()))) {
            const godot::Array transition = t;
            out.transitions.push_back({std::string(String(transition[0]).utf8().get_data()),
                                       static_cast<int>(transition[1])});
        }
        def.states.push_back(std::move(out));
    }
    player->finish(model, std::move(def));
    return player;
}

void SkydotBehavior::bind_tracks(ClipPose& pose, const std::unordered_map<std::string, godot::Node3D*>& nodes) {
    if (!pose.animation.is_valid()) {
        return;
    }
    for (int t = 0; t < pose.animation->get_track_count(); ++t) {
        const godot::NodePath track_path = pose.animation->track_get_path(t);
        if (track_path.get_subname_count() == 0) {
            continue;
        }
        const std::string bone = ascii_lower(std::string(String(track_path.get_subname(0)).utf8().get_data()));
        const auto node = nodes.find(bone);
        if (node == nodes.end()) {
            continue;
        }
        auto target = std::ranges::find(pose.targets, node->second, &Target::node);
        if (target == pose.targets.end()) {
            pose.targets.push_back({node->second, -1, -1});
            target = pose.targets.end() - 1;
        }
        if (pose.animation->track_get_type(t) == godot::Animation::TYPE_POSITION_3D) {
            target->position = t;
        } else if (pose.animation->track_get_type(t) == godot::Animation::TYPE_ROTATION_3D) {
            target->rotation = t;
        }
    }
}

void SkydotBehavior::finish(godot::Node* model, BehaviorDef def) {
    machine_ = std::make_unique<BehaviorMachine>(std::move(def));
    // The model's NIF animator leaves the nodes this graph poses to it.
    std::vector<godot::Node3D*> owned;
    for (const ClipPose& pose : poses_) {
        for (const Target& target : pose.targets) {
            owned.push_back(target.node);
        }
    }
    if (auto* animator = godot::Object::cast_to<SkydotAnimator>(model->get_node_or_null("SkydotAnimator"))) {
        animator->mask_nodes(owned);
    }
    model->add_child(this);
    // The start state shows from the first frame.
    BehaviorOutput out;
    machine_->start(out);
    report(out);
    apply();
    set_process(true);
}

void SkydotBehavior::report(const BehaviorOutput& out) {
    for (const BehaviorChange& change : out.changes) {
        char line[256];
        std::snprintf(line, sizeof line, "0x%08llX behaviour: %s -> %s on %s", static_cast<unsigned long long>(ref_),
                      change.from.empty() ? "(start)" : change.from.c_str(), change.to.c_str(),
                      change.event.empty() ? "start" : change.event.c_str());
        godot::UtilityFunctions::print(String::utf8(line));
    }
    for (const std::string& event : out.events) {
        emit_signal("event_raised", String::utf8(event.c_str()));
    }
}

void SkydotBehavior::apply() {
    if (!machine_) {
        return;
    }
    // A state without a clip holds the last pose: nothing else writes these nodes.
    const int clip = machine_->pose_clip();
    if (clip < 0 || static_cast<std::size_t>(clip) >= poses_.size()) {
        return;
    }
    // Written every frame, a held pose too: the model's NIF animator writes
    // its autoplay sequences (rest keys) to the same nodes every frame.
    const float time = machine_->pose_time();
    const ClipPose& pose = poses_[static_cast<std::size_t>(clip)];
    if (!pose.animation.is_valid()) {
        return;
    }
    const double t = std::clamp<double>(static_cast<double>(time), 0.0, pose.animation->get_length());
    for (const Target& target : pose.targets) {
        // The nodes are in the model this node is a child of: freed together.
        if (target.position >= 0) {
            target.node->set_position(pose.animation->position_track_interpolate(target.position, t));
        }
        if (target.rotation >= 0) {
            target.node->set_quaternion(pose.animation->rotation_track_interpolate(target.rotation, t));
        }
    }
}

bool SkydotBehavior::send_event(const String& event) {
    if (!machine_) {
        return false;
    }
    BehaviorOutput out;
    machine_->send(String(event).utf8().get_data(), out);
    report(out);
    apply();
    // Taken only when the graph now plays a Havok clip: states driven by
    // other generators (BGSGamebryoSequenceGenerator on doors, not read) leave
    // the event to the model's own NIF animations.
    const BehaviorState* state = machine_->state();
    return (!out.changes.empty() || out.accepted) && state != nullptr && state->clip >= 0;
}

bool SkydotBehavior::play_event(godot::Node* model, const String& event) {
    if (model == nullptr) {
        return false;
    }
    if (auto* graph = godot::Object::cast_to<SkydotBehavior>(model->find_child("SkydotBehavior", true, false));
        graph != nullptr && graph->send_event(event)) {
        return true;
    }
    auto* animator = godot::Object::cast_to<SkydotAnimator>(model->get_node_or_null("SkydotAnimator"));
    return animator != nullptr && animator->play(event);
}

String SkydotBehavior::get_state() const {
    const BehaviorState* state = machine_ ? machine_->state() : nullptr;
    return state != nullptr ? String::utf8(state->name.c_str()) : String();
}

void SkydotBehavior::_process(double delta) { advance(delta); }

void SkydotBehavior::advance(double delta) {
    if (!machine_) {
        return;
    }
    BehaviorOutput out;
    machine_->advance(static_cast<float>(delta), out);
    report(out);
    apply();
}

void SkydotBehavior::_bind_methods() {
    godot::ClassDB::bind_static_method("SkydotBehavior", godot::D_METHOD("attach", "model", "pack", "ref"),
                                       &SkydotBehavior::attach);
    godot::ClassDB::bind_static_method("SkydotBehavior", godot::D_METHOD("attach_graph", "model", "graph", "ref"),
                                       &SkydotBehavior::attach_graph);
    godot::ClassDB::bind_static_method("SkydotBehavior", godot::D_METHOD("play_event", "model", "event"),
                                       &SkydotBehavior::play_event);
    godot::ClassDB::bind_method(godot::D_METHOD("send_event", "event"), &SkydotBehavior::send_event);
    godot::ClassDB::bind_method(godot::D_METHOD("advance", "seconds"), &SkydotBehavior::advance);
    godot::ClassDB::bind_method(godot::D_METHOD("get_state"), &SkydotBehavior::get_state);
    ADD_SIGNAL(godot::MethodInfo("event_raised", godot::PropertyInfo(godot::Variant::STRING, "event")));
}

} // namespace skydot
