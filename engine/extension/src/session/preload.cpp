// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotStreamer: building the place behind the nearest load door ahead (see
// streamer.hpp).
#include "session/held_place.hpp"
#include "session/streamer.hpp"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

using godot::Dictionary;
using godot::Node;
using godot::Node3D;
using godot::Ref;
using godot::Vector2i;
using godot::Vector3;

namespace skydot {
namespace {

std::int64_t now_usec() { return static_cast<std::int64_t>(godot::Time::get_singleton()->get_ticks_usec()); }

Node3D* node_of(std::uint64_t id) {
    return godot::Object::cast_to<Node3D>(godot::ObjectDB::get_instance(id));
}

} // namespace

void SkydotStreamer::register_doors(Node* cell) {
    // The reference nodes under `cell` (not those inside another reference).
    std::vector<Node*> pending{cell};
    std::vector<Node*> refs;
    while (!pending.empty()) {
        const Node* parent = pending.back();
        pending.pop_back();
        for (std::int32_t i = 0; i < parent->get_child_count(); ++i) {
            Node* child = parent->get_child(i);
            if (child->has_meta("skydot_ref")) {
                refs.push_back(child);
            } else if (child->get_child_count() > 0) {
                pending.push_back(child);
            }
        }
    }
    for (Node* node : refs) {
        const auto ref = static_cast<std::uint32_t>(static_cast<std::int64_t>(node->get_meta("skydot_ref")));
        if (world_->data().door_ptr(ref) == nullptr) {
            continue;
        }
        const auto known = std::ranges::find(load_doors_, ref, &std::pair<std::uint32_t, std::uint64_t>::first);
        if (known != load_doors_.end()) {
            known->second = node->get_instance_id();
        } else {
            load_doors_.emplace_back(ref, node->get_instance_id());
        }
    }
}

Dictionary SkydotStreamer::get_load_doors() const {
    Dictionary out;
    for (const auto& [ref, id] : load_doors_) {
        out[static_cast<std::int64_t>(ref)] = node_of(id);
    }
    return out;
}

bool SkydotStreamer::toggle_preload() {
    preload_enabled_ = !preload_enabled_;
    if (!preload_enabled_) {
        drop_prepared();
    }
    return preload_enabled_;
}

void SkydotStreamer::preload_step(const Vector3& feet) {
    if (!preload_enabled_) {
        return;
    }
    if (--scan_ <= 0) {
        scan_ = PRELOAD_SCAN_FRAMES;
        std::uint32_t nearest = 0;
        double nearest_distance = preload_distance_;
        const std::uint32_t door = current_.is_valid() ? static_cast<std::uint32_t>(current_->door_) : 0;
        double current_distance = std::numeric_limits<double>::infinity();
        for (auto it = load_doors_.begin(); it != load_doors_.end();) {
            Node3D* node = node_of(it->second);
            if (node == nullptr || node->is_queued_for_deletion()) {
                it = load_doors_.erase(it);
                continue;
            }
            const auto d = static_cast<double>(node->get_global_position().distance_to(feet));
            if (it->first == door) {
                current_distance = d;
            }
            if (d < nearest_distance) {
                nearest = it->first;
                nearest_distance = d;
            }
            ++it;
        }
        if (nearest != 0 && nearest != door) {
            drop_prepared();
            current_ = begin_preparation(world_->get_door(nearest));
        } else if (nearest == 0 && door != 0 && current_distance > preload_distance_ * 1.5) {
            drop_prepared();
        }
    }
    if (current_.is_null() || current_->done_ || streaming_) {
        return;
    }
    advance_preparation(PRELOAD_BUDGET_USEC);
}

Ref<SkydotPreparation> SkydotStreamer::take_prepared(const Dictionary& door) {
    Ref<SkydotPreparation> taken;
    if (current_.is_valid() && current_->destination_ == static_cast<std::int64_t>(door["destination"])) {
        taken = current_;
        current_.unref();
        godot::UtilityFunctions::print(
            godot::String("using what was prepared (") + (taken->done_ ? "complete" : "in part") + ")");
    }
    drop_prepared();
    return taken;
}

void SkydotStreamer::drop_prepared() {
    const Ref<SkydotPreparation> p = current_;
    current_.unref();
    if (p.is_null()) {
        return;
    }
    if (Node3D* root = node_of(p->root_)) {
        root->queue_free();
    }
    for (const auto& [key, id] : p->cells_) {
        if (Node3D* cell = node_of(id)) {
            cell->queue_free();
        }
    }
    if (Node3D* lod = node_of(p->lod_)) {
        lod->queue_free();
    }
}

Ref<SkydotPreparation> SkydotStreamer::begin_preparation(const Dictionary& door) {
    Ref<SkydotPreparation> p;
    p.instantiate();
    p->door_ = door["ref"];
    p->destination_ = door["destination"];
    p->started_usec_ = static_cast<std::uint64_t>(now_usec());
    if (ai_.is_valid()) {
        ai_->begin_placing(); // where actors are, worked out over the next frames
    }
    if (static_cast<bool>(door["destination_interior"])) {
        p->interior_ = true;
        p->cell_ = door["destination_cell"];
        return p;
    }
    if (static_cast<std::int64_t>(door["destination_world"]) == 0) {
        return {};
    }
    p->world_ = door["destination_world"];
    const godot::Transform3D arrival = door["arrival"];
    p->eye_ = arrival.origin + Vector3(0, static_cast<float>(eye_height_), 0);
    const Vector2i centre = cell_at(arrival.origin);
    for (std::int64_t dy = -radius_; dy <= radius_; ++dy) {
        for (std::int64_t dx = -radius_; dx <= radius_; ++dx) {
            p->keys_.emplace_back(static_cast<std::int32_t>(centre.x + dx), static_cast<std::int32_t>(centre.y + dy));
        }
    }
    std::ranges::stable_sort(p->keys_, [&](const CellKey& a, const CellKey& b) {
        const auto distance = [&](const CellKey& k) {
            const std::int64_t x = k.first - centre.x;
            const std::int64_t y = k.second - centre.y;
            return x * x + y * y;
        };
        return distance(a) < distance(b);
    });
    if (SkydotLod* lod = make_lod(p->world_)) {
        p->lod_ = lod->get_instance_id();
        held_place::hold(host(), lod);
    }
    return p;
}

void SkydotStreamer::advance_preparation(std::int64_t budget_usec) {
    const auto started = now_usec();
    SkydotPreparation& p = *current_.ptr();
    if (p.interior_) {
        Node3D* root = node_of(p.root_);
        if (root == nullptr) {
            if (world_->request_cell(p.cell_) > 0) {
                return;
            }
            root = world_->begin_cell(p.cell_);
            if (root != nullptr) {
                p.root_ = root->get_instance_id();
                held_place::hold(host(), root);
            }
        }
        p.done_ = world_->continue_build_static(root, budget_usec);
    } else {
        bool complete = true;
        for (const CellKey& key : p.keys_) {
            const std::int64_t left = budget_usec - (now_usec() - started);
            if (left <= 0) {
                complete = false;
                break;
            }
            if (p.placed_.contains(key)) {
                continue;
            }
            if (!p.cells_.contains(key)) {
                if (world_->request_exterior(p.world_, key.first, key.second) != 0) {
                    complete = false;
                    continue;
                }
                Node3D* begun = world_->begin_exterior(p.world_, key.first, key.second);
                p.cells_[key] = begun != nullptr ? begun->get_instance_id() : 0;
                if (begun != nullptr) {
                    held_place::hold(host(), begun);
                }
            }
            Node3D* cell = node_of(p.cells_[key]);
            if (cell == nullptr || world_->continue_build_static(cell, left)) {
                p.placed_.insert(key);
            } else {
                complete = false;
            }
        }
        if (SkydotLod* lod = p.get_lod(); lod != nullptr && lod->update(p.eye_, LOD_BUDGET_USEC) > 0) {
            complete = false;
        }
        p.done_ = complete;
    }
    if (p.done_) {
        char text[96];
        std::snprintf(text, sizeof(text), "prepared what is behind 0x%08X in %.0f ms",
                      static_cast<unsigned>(p.door_), static_cast<double>(now_usec() - static_cast<std::int64_t>(p.started_usec_)) / 1000.0);
        godot::UtilityFunctions::print(godot::String::utf8(text));
    }
}

} // namespace skydot
