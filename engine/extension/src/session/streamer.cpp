// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotStreamer: the bindings and the streaming of a worldspace's cells (see
// streamer.hpp); the preloading is in preload.cpp.
#include "session/streamer.hpp"
#include "actors/actor.hpp"

#include "data/coordinates.hpp"
#include "physics/collision.hpp"
#include "session/held_place.hpp"

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/primitive_mesh.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

using godot::Node3D;
using godot::Vector2i;
using godot::Vector3;

namespace skydot {
namespace {

constexpr double k_cell_units = SkydotWorld::CELL_UNITS;

std::int64_t now_usec() { return static_cast<std::int64_t>(godot::Time::get_singleton()->get_ticks_usec()); }

/// The squared distance in cells between two grid squares.
std::int64_t distance_squared(const CellKey& a, const CellKey& b) {
    const std::int64_t dx = a.first - b.first;
    const std::int64_t dy = a.second - b.second;
    return dx * dx + dy * dy;
}

/// How many cells apart two grid squares are, counting a step to a corner as one.
std::int64_t chebyshev(const CellKey& a, const CellKey& b) {
    return std::max(std::abs(static_cast<std::int64_t>(a.first) - b.first),
                    std::abs(static_cast<std::int64_t>(a.second) - b.second));
}

/// Nearest `centre` first; cells as far apart keep their order.
void sort_nearest(std::vector<CellKey>& keys, const CellKey& centre) {
    std::stable_sort(keys.begin(), keys.end(), [&](const CellKey& a, const CellKey& b) {
        return distance_squared(a, centre) < distance_squared(b, centre);
    });
}

} // namespace

void SkydotPreparation::_bind_methods() {
    using godot::ClassDB;
    using godot::D_METHOD;
    using godot::PropertyInfo;
    ClassDB::bind_method(D_METHOD("get_door"), &SkydotPreparation::get_door);
    ClassDB::bind_method(D_METHOD("get_destination"), &SkydotPreparation::get_destination);
    ClassDB::bind_method(D_METHOD("is_done"), &SkydotPreparation::is_done);
    ClassDB::bind_method(D_METHOD("is_interior"), &SkydotPreparation::is_interior);
    ClassDB::bind_method(D_METHOD("get_root"), &SkydotPreparation::get_root);
    ClassDB::bind_method(D_METHOD("get_lod"), &SkydotPreparation::get_lod);
    ADD_PROPERTY(PropertyInfo(godot::Variant::INT, "door"), "", "get_door");
    ADD_PROPERTY(PropertyInfo(godot::Variant::INT, "destination"), "", "get_destination");
    ADD_PROPERTY(PropertyInfo(godot::Variant::BOOL, "done"), "", "is_done");
    ADD_PROPERTY(PropertyInfo(godot::Variant::BOOL, "interior"), "", "is_interior");
    ADD_PROPERTY(PropertyInfo(godot::Variant::OBJECT, "root", godot::PROPERTY_HINT_NODE_TYPE, "Node3D"), "",
                 "get_root");
    ADD_PROPERTY(PropertyInfo(godot::Variant::OBJECT, "lod", godot::PROPERTY_HINT_NODE_TYPE, "SkydotLod"), "",
                 "get_lod");
}

Node3D* SkydotPreparation::get_root() const {
    return godot::Object::cast_to<Node3D>(godot::ObjectDB::get_instance(root_));
}

SkydotLod* SkydotPreparation::get_lod() const {
    return godot::Object::cast_to<SkydotLod>(godot::ObjectDB::get_instance(lod_));
}

void SkydotStreamer::_bind_methods() {
    using godot::ClassDB;
    using godot::D_METHOD;
    using godot::MethodInfo;
    using godot::PropertyInfo;
    using godot::Variant;
    ClassDB::bind_method(D_METHOD("setup", "world", "pack", "host", "camera", "ai"), &SkydotStreamer::setup);

#define SKYDOT_PROPERTY(name, type)                                                               \
    ClassDB::bind_method(D_METHOD("set_" #name, "value"), &SkydotStreamer::set_##name);         \
    ClassDB::bind_method(D_METHOD("get_" #name), &SkydotStreamer::get_##name);                  \
    ADD_PROPERTY(PropertyInfo(Variant::type, #name), "set_" #name, "get_" #name)

    SKYDOT_PROPERTY(radius, INT);
    SKYDOT_PROPERTY(build_budget_usec, INT);
    SKYDOT_PROPERTY(lod_split, FLOAT);
    SKYDOT_PROPERTY(lod_enabled, BOOL);
    SKYDOT_PROPERTY(large_refs, BOOL);
    SKYDOT_PROPERTY(large_ref_radius, INT);
    SKYDOT_PROPERTY(eye_height, FLOAT);
    SKYDOT_PROPERTY(preload_enabled, BOOL);
    SKYDOT_PROPERTY(preload_distance, FLOAT);
    SKYDOT_PROPERTY(max_usec, INT);
#undef SKYDOT_PROPERTY
    ClassDB::bind_method(D_METHOD("set_tree_distance", "cells"), &SkydotStreamer::set_tree_distance);

    ClassDB::bind_method(D_METHOD("get_world_id"), &SkydotStreamer::get_world_id);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "world_id"), "", "get_world_id");
    ClassDB::bind_method(D_METHOD("is_streaming"), &SkydotStreamer::is_streaming);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "streaming"), "", "is_streaming");
    ClassDB::bind_method(D_METHOD("is_lod_busy"), &SkydotStreamer::is_lod_busy);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "lod_busy"), "", "is_lod_busy");
    ClassDB::bind_method(D_METHOD("set_lod", "lod"), &SkydotStreamer::set_lod);
    ClassDB::bind_method(D_METHOD("get_lod"), &SkydotStreamer::get_lod);
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "lod", godot::PROPERTY_HINT_NODE_TYPE, "SkydotLod"), "set_lod",
                 "get_lod");
    ClassDB::bind_method(D_METHOD("get_loaded_cell", "key"), &SkydotStreamer::get_loaded_cell);
    ClassDB::bind_method(D_METHOD("get_loaded_cells"), &SkydotStreamer::get_loaded_cells);

    ClassDB::bind_static_method("SkydotStreamer", D_METHOD("cell_at", "position"), &SkydotStreamer::cell_at);
    ClassDB::bind_method(D_METHOD("camera_cell"), &SkydotStreamer::camera_cell);
    ClassDB::bind_method(D_METHOD("view_distance"), &SkydotStreamer::view_distance);
    ClassDB::bind_method(D_METHOD("start", "world_id"), &SkydotStreamer::start);
    ClassDB::bind_method(D_METHOD("clear"), &SkydotStreamer::clear);
    ClassDB::bind_method(D_METHOD("make_lod", "world_id"), &SkydotStreamer::make_lod);
    ClassDB::bind_method(D_METHOD("adopt", "prepared"), &SkydotStreamer::adopt);
    ClassDB::bind_method(D_METHOD("update"), &SkydotStreamer::update);
    ClassDB::bind_method(D_METHOD("load_everything"), &SkydotStreamer::load_everything);
    ClassDB::bind_method(D_METHOD("step"), &SkydotStreamer::step);
    ClassDB::bind_method(D_METHOD("hold_player", "player", "fading"), &SkydotStreamer::hold_player);
    ClassDB::bind_method(D_METHOD("scale_lod_split", "factor"), &SkydotStreamer::scale_lod_split);
    ClassDB::bind_method(D_METHOD("change_radius", "by"), &SkydotStreamer::change_radius);

    ClassDB::bind_method(D_METHOD("register_doors", "cell"), &SkydotStreamer::register_doors);
    ClassDB::bind_method(D_METHOD("get_load_doors"), &SkydotStreamer::get_load_doors);
    ClassDB::bind_method(D_METHOD("toggle_preload"), &SkydotStreamer::toggle_preload);
    ClassDB::bind_method(D_METHOD("preload_step", "feet"), &SkydotStreamer::preload_step);
    ClassDB::bind_method(D_METHOD("get_preparation"), &SkydotStreamer::get_preparation);
    ClassDB::bind_method(D_METHOD("take_prepared", "door"), &SkydotStreamer::take_prepared);
    ClassDB::bind_method(D_METHOD("drop_prepared"), &SkydotStreamer::drop_prepared);
    ClassDB::bind_static_method("SkydotStreamer", D_METHOD("release_held", "node"),
                                &SkydotStreamer::release_held);

    ADD_SIGNAL(MethodInfo("cell_finished", PropertyInfo(Variant::OBJECT, "cell", godot::PROPERTY_HINT_NODE_TYPE, "Node3D"),
                          PropertyInfo(Variant::INT, "cell_id")));
    ADD_SIGNAL(MethodInfo("cell_dropped", PropertyInfo(Variant::INT, "cell_id")));
}

void SkydotStreamer::setup(const godot::Ref<SkydotWorld>& world, const godot::Ref<SkydotPack>& pack,
                           Node3D* host, godot::Camera3D* camera, const godot::Ref<SkydotAi>& ai) {
    world_ = world;
    pack_ = pack;
    ai_ = ai;
    host_ = host->get_instance_id();
    camera_ = camera->get_instance_id();
}

Node3D* SkydotStreamer::host() const {
    return godot::Object::cast_to<Node3D>(godot::ObjectDB::get_instance(host_));
}

godot::Camera3D* SkydotStreamer::camera() const {
    return godot::Object::cast_to<godot::Camera3D>(godot::ObjectDB::get_instance(camera_));
}

void SkydotStreamer::set_large_refs(bool enabled) {
    large_refs_ = enabled;
    if (SkydotLod* lod = get_lod()) {
        lod->set_large_refs(enabled);
    }
}

void SkydotStreamer::set_large_ref_radius(std::int64_t cells) {
    large_ref_radius_ = std::clamp<std::int64_t>(cells, 0, 32);
    if (SkydotLod* lod = get_lod()) {
        lod->set_large_ref_radius(large_ref_radius_);
    }
}

void SkydotStreamer::set_lod(SkydotLod* lod) { lod_ = lod != nullptr ? lod->get_instance_id() : 0; }

SkydotLod* SkydotStreamer::get_lod() const {
    return godot::Object::cast_to<SkydotLod>(godot::ObjectDB::get_instance(lod_));
}

Node3D* SkydotStreamer::get_loaded_cell(const Vector2i& key) const {
    const auto it = loaded_.find({key.x, key.y});
    return it != loaded_.end() ? godot::Object::cast_to<Node3D>(godot::ObjectDB::get_instance(it->second)) : nullptr;
}

godot::Array SkydotStreamer::get_loaded_cells() const {
    godot::Array out;
    for (const auto& [key, id] : loaded_) {
        if (Node3D* cell = godot::Object::cast_to<Node3D>(godot::ObjectDB::get_instance(id))) {
            out.push_back(cell);
        }
    }
    return out;
}

Vector2i SkydotStreamer::cell_at(const Vector3& position) {
    const Vector3 p = position / static_cast<float>(SkydotWorld::UNIT_SCALE);
    return {static_cast<std::int32_t>(std::floor(static_cast<double>(p.x) / k_cell_units)),
            static_cast<std::int32_t>(std::floor(-static_cast<double>(p.z) / k_cell_units))};
}

Vector2i SkydotStreamer::camera_cell() const { return cell_at(camera()->get_position()); }

double SkydotStreamer::view_distance() const {
    return static_cast<double>(radius_ + 1) * k_cell_units * SkydotWorld::UNIT_SCALE * 1.5;
}

void SkydotStreamer::start(std::int64_t id) {
    world_id_ = id;
    batch_ = true;
}

void SkydotStreamer::clear() {
    for (const auto& [key, id] : loaded_) {
        if (auto* cell = godot::Object::cast_to<Node3D>(godot::ObjectDB::get_instance(id))) {
            cell->queue_free();
        }
    }
    loaded_.clear();
    cell_currents_.clear();
    for (const auto& [key, id] : building_) {
        if (auto* cell = godot::Object::cast_to<Node3D>(godot::ObjectDB::get_instance(id))) {
            cell->queue_free();
        }
    }
    building_.clear();
    awaiting_.clear();
    batch_ = false;
    load_doors_.clear();
    world_id_ = 0;
    lod_ = 0;
    streaming_ = false;
    lod_busy_ = false;
}

SkydotLod* SkydotStreamer::make_lod(std::int64_t id) {
    if (!lod_enabled_) {
        return nullptr;
    }
    auto* made = memnew(SkydotLod);
    made->set_large_ref_radius(large_ref_radius_);
    made->set_large_refs(large_refs_); // before setup, which makes the layer
    if (made->setup(pack_, world_, id) != godot::OK) {
        godot::UtilityFunctions::print("no LOD: ", made->get_error());
        memdelete(made);
        return nullptr;
    }
    made->set_name("lod");
    made->set_split_distance(lod_split_);
    if (tree_distance_) {
        made->set_tree_distance(*tree_distance_);
    }
    return made;
}

void SkydotStreamer::adopt(const godot::Ref<SkydotPreparation>& prepared) {
    if (prepared.is_null()) {
        return;
    }
    const Vector2i at = camera_cell();
    const CellKey centre{at.x, at.y};
    for (const auto& [key, id] : prepared->cells_) {
        auto* cell = godot::Object::cast_to<Node3D>(godot::ObjectDB::get_instance(id));
        if (cell == nullptr) {
            loaded_[key] = 0;
            continue;
        }
        cell->set_meta("skydot_streamed", true);
        if (key == centre) {
            held_place::release(cell);
            cell->set_visible(false); // until finished
        }
        building_.emplace_back(key, id);
    }
}

void SkydotStreamer::update() {
    if (world_id_ == 0) {
        return;
    }
    const auto started = now_usec();
    streaming_ = step();
    batch_ = batch_ && streaming_;
    release_actors();
    if (SkydotLod* lod = get_lod()) {
        lod_busy_ = lod->update(camera()->get_global_position(), LOD_BUDGET_USEC) > 0;
    }
    max_usec_ = std::max(max_usec_, now_usec() - started);
}

void SkydotStreamer::load_everything() {
    while (step()) {
        godot::OS::get_singleton()->delay_msec(5);
    }
    batch_ = false;
    release_actors();
    while (get_lod() != nullptr && get_lod()->update(camera()->get_global_position(), 1000000) > 0) {
        godot::OS::get_singleton()->delay_msec(5);
    }
}

void SkydotStreamer::rescue_flyers(Node3D* cell) {
    // A flying actor is not freed with its cell (a dragon circling over a cell that scrolls away):
    // it moves, keeping its place in the world, to the loaded cell under it, else to the one under
    // the camera.
    for (const godot::Variant& child : cell->find_children("*", "SkydotActor", true, false)) {
        auto* actor = godot::Object::cast_to<SkydotActor>(child);
        if (actor == nullptr || !actor->is_flying()) {
            continue;
        }
        Node3D* to = get_loaded_cell(cell_at(actor->get_global_position()));
        if (to == nullptr || to == cell) {
            to = get_loaded_cell(camera_cell());
        }
        if (to == nullptr || to == cell) {
            continue;
        }
        actor->reparent(to, true);
        // The scripts of its reference stay loaded (SkydotPapyrus::detach_cell asks SkydotAi).
        actor->set_meta("skydot_rescued", true);
    }
}

bool SkydotStreamer::step() {
    const Vector2i at = camera_cell();
    const CellKey centre{at.x, at.y};
    SkydotLod* lod = get_lod();
    bool dropped = false;
    for (auto it = loaded_.begin(); it != loaded_.end();) {
        if (chebyshev(it->first, centre) <= radius_ + 1) {
            ++it;
            continue;
        }
        if (it->second != 0) {
            if (auto* cell = godot::Object::cast_to<Node3D>(godot::ObjectDB::get_instance(it->second))) {
                rescue_flyers(cell);
                emit_signal("cell_dropped", world_->get_exterior_cell(world_id_, it->first.first, it->first.second));
                cell->queue_free();
            }
            dropped = true;
            if (lod != nullptr) {
                lod->set_cell_loaded(it->first.first, it->first.second, false);
            }
        }
        it = loaded_.erase(it);
    }
    for (auto it = building_.begin(); it != building_.end();) {
        if (chebyshev(it->first, centre) > radius_ + 1) {
            if (auto* cell = godot::Object::cast_to<Node3D>(godot::ObjectDB::get_instance(it->second))) {
                cell->queue_free();
            }
            it = building_.erase(it);
        } else {
            ++it;
        }
    }
    if (dropped) {
        world_->call_deferred("trim_cache");
    }

    const auto started = now_usec();
    std::vector<CellKey> order;
    order.reserve(building_.size());
    for (const auto& entry : building_) {
        order.push_back(entry.first);
    }
    sort_nearest(order, centre);
    for (const CellKey& key : order) {
        const std::int64_t left = build_budget_usec_ - (now_usec() - started);
        if (left <= 0) {
            return true;
        }
        const auto entry = std::ranges::find(building_, key, &std::pair<CellKey, std::uint64_t>::first);
        auto* cell = godot::Object::cast_to<Node3D>(godot::ObjectDB::get_instance(entry->second));
        if (cell == nullptr) {
            building_.erase(entry);
        } else if (world_->continue_build(cell, left)) {
            building_.erase(entry);
            finish_cell(key, cell);
        }
    }

    std::vector<CellKey> wanted;
    for (std::int64_t dy = -radius_; dy <= radius_; ++dy) {
        for (std::int64_t dx = -radius_; dx <= radius_; ++dx) {
            const CellKey key{static_cast<std::int32_t>(centre.first + dx),
                              static_cast<std::int32_t>(centre.second + dy)};
            if (!loaded_.contains(key) && std::ranges::none_of(building_, [&](const auto& e) { return e.first == key; })) {
                wanted.push_back(key);
            }
        }
    }
    if (wanted.empty()) {
        return !building_.empty();
    }
    sort_nearest(wanted, centre);

    constexpr std::size_t k_begun_per_frame = 4;
    for (std::size_t i = 0; i < std::min(wanted.size(), k_begun_per_frame); ++i) {
        const CellKey key = wanted[i];
        if (world_->request_exterior(world_id_, key.first, key.second) != 0) {
            continue;
        }
        const std::int64_t left = build_budget_usec_ - (now_usec() - started);
        if (left <= 0) {
            break;
        }
        Node3D* cell = world_->begin_exterior(world_id_, key.first, key.second);
        if (cell == nullptr) {
            loaded_[key] = 0;
            continue;
        }
        cell->set_visible(false);
        cell->set_meta("skydot_streamed", true); // its actors wait for the ground of the cells next to it
        host()->add_child(cell);
        if (world_->continue_build(cell, left)) {
            finish_cell(key, cell);
        } else {
            building_.emplace_back(key, cell->get_instance_id());
        }
    }
    return true;
}

void SkydotStreamer::finish_cell(const CellKey& key, Node3D* cell) {
    if (cell->get_process_mode() == godot::Node::PROCESS_MODE_DISABLED) {
        held_place::release(cell); // prepared behind a door
    }
    cell->set_visible(true);
    if (ai_.is_valid()) {
        ai_->attach_built(cell);
    }
    godot::UtilityFunctions::print("cell ", Vector2i(key.first, key.second), ": ", cell->get_meta("skydot_stats"));
    emit_signal("cell_finished", cell, world_->get_exterior_cell(world_id_, key.first, key.second));
    register_doors(cell);
    if (SkydotLod* lod = get_lod()) {
        lod->set_cell_loaded(key.first, key.second, true);
    }
    loaded_[key] = cell->get_instance_id();
    for (const godot::Variant& child : cell->find_children("*", "SkydotActor", true, false)) {
        if (auto* actor = godot::Object::cast_to<SkydotActor>(child); actor != nullptr && actor->is_awaiting_world()) {
            awaiting_.push_back(actor->get_instance_id());
        }
    }
}

void SkydotStreamer::release_actors() {
    if (batch_) {
        return; // the cells entered with `start` are all built first
    }
    for (auto it = awaiting_.begin(); it != awaiting_.end();) {
        auto* actor = godot::Object::cast_to<SkydotActor>(godot::ObjectDB::get_instance(*it));
        if (actor == nullptr || !actor->is_awaiting_world()) {
            it = awaiting_.erase(it);
            continue;
        }
        // Its own cell and every cell around it: a floor, deck or bridge of a neighbour may reach
        // over the border to where it stands.
        const Vector2i at = cell_at(actor->get_global_position());
        bool ready = true;
        for (std::int32_t dy = -1; dy <= 1 && ready; ++dy) {
            for (std::int32_t dx = -1; dx <= 1 && ready; ++dx) {
                const CellKey key{at.x + dx, at.y + dy};
                // A square the worldspace has no cell for has nothing to wait for.
                ready = loaded_.contains(key) || world_->get_exterior_cell(world_id_, key.first, key.second) == 0;
            }
        }
        if (ready) {
            actor->set_awaiting_world(false);
            it = awaiting_.erase(it);
        } else {
            ++it;
        }
    }
}

void SkydotStreamer::hold_player(SkydotPlayer* player, bool fading) {
    const Vector3 feet = player->get_global_position();
    double surface = -std::numeric_limits<double>::infinity();
    Vector3 current;
    if (world_id_ == 0) {
        player->set_hold(fading);
    } else {
        const Vector2i at = camera_cell();
        const auto it = loaded_.find({at.x, at.y});
        player->set_hold((it == loaded_.end() && !player->get_fly()) || fading);
        Node3D* cell = it != loaded_.end() ? godot::Object::cast_to<Node3D>(godot::ObjectDB::get_instance(it->second)) : nullptr;
        auto* water = cell != nullptr ? godot::Object::cast_to<Node3D>(cell->get_node_or_null(godot::NodePath("Water"))) : nullptr;
        if (water != nullptr) {
            surface = static_cast<double>(water->get_global_position().y);
            current = cell_current({at.x, at.y}, water, feet);
        }
    }
    const bool placed = placed_water_at(feet, surface, current);
    if (placed || std::isfinite(surface)) {
        player->set_water_height(surface);
        player->set_water_current(current);
    } else {
        player->clear_water();
    }
}

// ---- Water ---------------------------------------------------------------

namespace {

/// A current in Havok units per second, game axes, as metres per second in Godot space.
Vector3 godot_velocity(const Vector3& havok) {
    return skyrim_position(havok * k_havok_scale) - skyrim_position(Vector3());
}

/// Skyrim's water layer (L_WATER): a placed water model's volume.
constexpr std::int64_t k_water_layer = 11;

/// The flowmap's fill outside the rivers (RG 120, 0: 99.6% of it on dry land in Skyrim.esm's).
bool flow_fill(const godot::Color& texel) { return texel.get_r8() == 120 && texel.get_g8() == 0; }

/// A flowmap texel's current at full scroll (assumed): RiverWaterFlowNE's NAM0 speed, Havok units
/// per second.
constexpr double k_flow_speed = 2.877;

} // namespace

void SkydotStreamer::register_water(std::uint32_t ref, godot::Node* node) {
    auto* placed = godot::Object::cast_to<Node3D>(node);
    if (placed == nullptr || !node->has_meta(k_phantoms_meta) ||
        std::ranges::any_of(placed_water_, [&](const PlacedWater& w) { return w.node == node->get_instance_id(); })) {
        return;
    }
    // The reference's XWCU (its first entry), else its water type's NAM0.
    Vector3 havok;
    const godot::Array currents = world_->get_ref_water_currents(ref);
    if (!currents.is_empty()) {
        const godot::Vector4 v = currents[0];
        havok = Vector3(v.x, v.y, v.z);
    } else if (const std::int64_t type = world_->get_ref_water_type(ref); type != 0) {
        havok = world_->get_water_velocity(type).get("linear", Vector3());
    }
    const godot::Array list = node->get_meta(k_phantoms_meta);
    for (std::int64_t i = 0; i < list.size(); ++i) {
        const godot::Dictionary d = list[i];
        if (static_cast<std::int64_t>(d.get("layer", 0)) == k_water_layer && godot::String(d.get("kind", "")) == "box") {
            placed_water_.push_back({node->get_instance_id(), d["transform"], d["half"], godot_velocity(havok)});
        }
    }
}

// The surface is the box's top face over the feet.
bool SkydotStreamer::placed_water_at(const Vector3& feet, double& surface, Vector3& current) {
    bool found = false;
    for (auto it = placed_water_.begin(); it != placed_water_.end();) {
        auto* node = godot::Object::cast_to<Node3D>(godot::ObjectDB::get_instance(it->node));
        if (node == nullptr || node->is_queued_for_deletion()) {
            it = placed_water_.erase(it);
            continue;
        }
        const godot::Transform3D box = node->get_global_transform() * it->box;
        const Vector3 p = box.affine_inverse().xform(feet);
        const Vector3& h = it->half;
        if (node->is_visible_in_tree() && std::abs(p.x) <= h.x && std::abs(p.y) <= h.y && std::abs(p.z) <= h.z) {
            // The top face: the box axis most upright, towards up.
            int up = 0;
            for (int k = 1; k < 3; ++k) {
                if (std::abs(box.basis.get_column(k).y) > std::abs(box.basis.get_column(up).y)) {
                    up = k;
                }
            }
            Vector3 face = p;
            face[up] = box.basis.get_column(up).y > 0 ? h[up] : -h[up];
            const double top = static_cast<double>(box.xform(face).y);
            if (top > surface) {
                surface = top;
                current = it->current;
                found = true;
            }
        }
        ++it;
    }
    return found;
}

// The cell's water type's NAM0; without one, the flowmap the water plane draws (decoration.cpp):
// RG the direction (2c - 1: x east, y north, as the water shader's rotation), A its scroll, taken
// as a fraction of k_flow_speed (assumed).
Vector3 SkydotStreamer::cell_current(const CellKey& key, Node3D* water, const Vector3& feet) {
    auto it = cell_currents_.find(key);
    if (it == cell_currents_.end()) {
        CellCurrent made;
        const std::int64_t type = world_->get_cell_water_type(world_->get_exterior_cell(world_id_, key.first, key.second));
        made.linear = godot_velocity(world_->get_water_velocity(type).get("linear", Vector3()));
        const auto* mesh = godot::Object::cast_to<godot::MeshInstance3D>(water);
        const godot::Ref<godot::PrimitiveMesh> plane = mesh != nullptr ? mesh->get_mesh() : godot::Ref<godot::Mesh>();
        const godot::Ref<godot::ShaderMaterial> material = plane.is_valid() ? plane->get_material() : godot::Ref<godot::Material>();
        if (made.linear.is_zero_approx() && material.is_valid() && material->get_shader_parameter("flow_enabled")) {
            const godot::Ref<godot::Texture2D> flow = material->get_shader_parameter("flow_tex");
            if (flow.is_valid()) {
                made.flow = flow->get_image();
            }
            if (made.flow.is_valid()) {
                made.flow = made.flow->duplicate();
                if (made.flow->is_compressed()) {
                    made.flow->decompress();
                }
                made.flow->convert(godot::Image::FORMAT_RGBA8);
            }
        }
        it = cell_currents_.emplace(key, made).first;
    }
    const CellCurrent& c = it->second;
    if (c.flow.is_null() || c.flow->is_empty()) {
        return c.linear;
    }
    // Rows run north to south.
    const Vector3 game = godot_to_skyrim(feet);
    const double u = (static_cast<double>(game.x) - key.first * k_cell_units) / k_cell_units;
    const double v = ((key.second + 1) * k_cell_units - static_cast<double>(game.y)) / k_cell_units;
    const int px = std::clamp(static_cast<int>(u * c.flow->get_width()), 0, c.flow->get_width() - 1);
    const int py = std::clamp(static_cast<int>(v * c.flow->get_height()), 0, c.flow->get_height() - 1);
    const godot::Color texel = c.flow->get_pixel(px, py);
    if (flow_fill(texel)) {
        return {};
    }
    const Vector3 direction(texel.r * 2 - 1, texel.g * 2 - 1, 0);
    return godot_velocity(direction * static_cast<godot::real_t>(static_cast<double>(texel.a) * k_flow_speed));
}

double SkydotStreamer::scale_lod_split(double factor) {
    lod_split_ = std::clamp(lod_split_ * factor, 0.5, 16.0);
    if (SkydotLod* lod = get_lod()) {
        lod->set_split_distance(lod_split_);
    }
    return lod_split_;
}

std::int64_t SkydotStreamer::change_radius(std::int64_t by) {
    radius_ = std::clamp<std::int64_t>(radius_ + by, 1, 8);
    if (get_lod() == nullptr) {
        camera()->set_far(static_cast<float>(view_distance()));
    }
    return radius_;
}

void SkydotStreamer::release_held(Node3D* node) { held_place::release(node); }

} // namespace skydot
