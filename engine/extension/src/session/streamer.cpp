// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotStreamer: the bindings and the streaming of a worldspace's cells (see
// streamer.hpp); the preloading is in preload.cpp.
#include "session/streamer.hpp"

#include "session/held_place.hpp"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

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

void SkydotStreamer::start(std::int64_t id) { world_id_ = id; }

void SkydotStreamer::clear() {
    for (const auto& [key, id] : loaded_) {
        if (auto* cell = godot::Object::cast_to<Node3D>(godot::ObjectDB::get_instance(id))) {
            cell->queue_free();
        }
    }
    loaded_.clear();
    for (const auto& [key, id] : building_) {
        if (auto* cell = godot::Object::cast_to<Node3D>(godot::ObjectDB::get_instance(id))) {
            cell->queue_free();
        }
    }
    building_.clear();
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
    if (SkydotLod* lod = get_lod()) {
        lod_busy_ = lod->update(camera()->get_global_position(), LOD_BUDGET_USEC) > 0;
    }
    max_usec_ = std::max(max_usec_, now_usec() - started);
}

void SkydotStreamer::load_everything() {
    while (step()) {
        godot::OS::get_singleton()->delay_msec(5);
    }
    while (get_lod() != nullptr && get_lod()->update(camera()->get_global_position(), 1000000) > 0) {
        godot::OS::get_singleton()->delay_msec(5);
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
}

void SkydotStreamer::hold_player(SkydotPlayer* player, bool fading) {
    if (world_id_ == 0) {
        player->set_hold(fading);
        player->clear_water();
        return;
    }
    const Vector2i at = camera_cell();
    const auto it = loaded_.find({at.x, at.y});
    player->set_hold((it == loaded_.end() && !player->get_fly()) || fading);
    Node3D* cell = it != loaded_.end() ? godot::Object::cast_to<Node3D>(godot::ObjectDB::get_instance(it->second)) : nullptr;
    auto* water = cell != nullptr ? godot::Object::cast_to<Node3D>(cell->get_node_or_null(godot::NodePath("Water"))) : nullptr;
    if (water != nullptr) {
        player->set_water_height(static_cast<double>(water->get_global_position().y));
    } else {
        player->clear_water();
    }
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
