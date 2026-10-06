// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/large_refs.hpp"

#include "data/large_refs.hpp"
#include "render/lod.hpp"

#include "skydot_formats/units.hpp"
#include "world_generated.h"

#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>

#include <algorithm>
#include <cmath>

using godot::Dictionary;
using godot::Ref;
using godot::String;
using godot::Vector3;

namespace wfb = bethconv::pack::wfb;

namespace skydot {

namespace {

constexpr auto k_cell_units = static_cast<double>(formats::k_cell_units);

std::int64_t now_usec() { return static_cast<std::int64_t>(godot::Time::get_singleton()->get_ticks_usec()); }

} // namespace

void SkydotLargeRefs::_bind_methods() {
    using godot::D_METHOD;
    godot::ClassDB::bind_method(D_METHOD("set_radius", "cells"), &SkydotLargeRefs::set_radius);
    godot::ClassDB::bind_method(D_METHOD("get_radius"), &SkydotLargeRefs::get_radius);
    godot::ClassDB::bind_method(D_METHOD("set_cell_built", "x", "y", "built"), &SkydotLargeRefs::set_cell_built);
    godot::ClassDB::bind_method(D_METHOD("update", "camera", "budget_usec"), &SkydotLargeRefs::update);
    godot::ClassDB::bind_method(D_METHOD("clear"), &SkydotLargeRefs::clear);
    godot::ClassDB::bind_method(D_METHOD("get_stats"), &SkydotLargeRefs::get_stats);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::INT, "radius"), "set_radius", "get_radius");
}

godot::Error SkydotLargeRefs::setup(const Ref<SkydotPack>& pack, const Ref<SkydotWorld>& world,
                                    std::int64_t world_id, SkydotLod* lod) {
    if (pack.is_null() || world.is_null() ||
        world->data().large_ref_count(static_cast<std::uint32_t>(world_id)) == 0) {
        return godot::ERR_UNAVAILABLE;
    }
    pack_ = pack;
    world_ = world;
    world_id_ = static_cast<std::uint32_t>(world_id);
    lod_ = lod;
    return godot::OK;
}

void SkydotLargeRefs::set_radius(std::int64_t cells) {
    const auto radius = std::clamp<std::int64_t>(cells, 0, 32);
    if (radius != radius_) {
        radius_ = radius;
        dirty_ = true;
    }
}

void SkydotLargeRefs::set_cell_built(std::int64_t x, std::int64_t y, bool is_built) {
    const auto k = key(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y));
    const bool changed = is_built ? built_cells_.insert(k).second : built_cells_.erase(k) > 0;
    if (changed) {
        dirty_ = true;
        marks_dirty_ = true;
    }
}

bool SkydotLargeRefs::built(std::int32_t x, std::int32_t y) const { return built_cells_.contains(key(x, y)); }

const SkydotLargeRefs::Info& SkydotLargeRefs::info(std::uint32_t index) {
    const auto found = infos_.find(index);
    if (found != infos_.end()) {
        return found->second;
    }
    Info out;
    if (const auto* ref = world_->data().large_ref(world_id_, index)) {
        const String model = world_->large_ref_model(*ref, &out.reason);
        if (!model.is_empty()) {
            const auto bytes = model.utf8();
            out.model.assign(bytes.get_data(), static_cast<std::size_t>(bytes.length()));
        }
    } else {
        out.reason = "no base";
    }
    if (out.model.empty()) {
        ++skipped_[out.reason != nullptr ? out.reason : "no model"];
    }
    return infos_.emplace(index, std::move(out)).first->second;
}

void SkydotLargeRefs::select(std::int32_t cx, std::int32_t cy, double x, double y) {
    const auto started = now_usec();
    const auto chosen = large_refs::select(world_->data(), world_id_, cx, cy, static_cast<std::int32_t>(radius_),
                                           [this](std::int32_t gx, std::int32_t gy) { return built(gx, gy); });
    candidates_.clear();
    wanted_.clear();
    for (const std::uint32_t index : chosen) {
        if (!info(index).model.empty()) {
            candidates_.push_back(index);
        }
    }
    const auto distance = [&](std::uint32_t index) {
        const auto& p = world_->data().large_ref(world_id_, index)->position();
        const double dx = static_cast<double>(p.x()) - x;
        const double dy = static_cast<double>(p.y()) - y;
        return dx * dx + dy * dy;
    };
    std::vector<std::pair<double, std::uint32_t>> order;
    order.reserve(candidates_.size());
    for (const std::uint32_t index : candidates_) {
        order.emplace_back(distance(index), index);
    }
    std::ranges::sort(order);
    for (std::size_t i = 0; i < order.size(); ++i) {
        candidates_[i] = order[i].second;
        wanted_.insert(order[i].second);
    }
    // Models load on the asset cache's threads, all asked for at once.
    if (const auto assets = pack_->assets()) {
        for (const std::uint32_t index : candidates_) {
            const std::string& model = info(index).model;
            if (requested_.insert(model).second) {
                assets->request(model);
            }
        }
    }
    // What nothing wants any more lets go of its model.
    std::set<std::string> needed;
    for (const std::uint32_t index : candidates_) {
        needed.insert(info(index).model);
    }
    std::erase_if(models_, [&](const auto& e) {
        return !needed.contains(e.first) && (e.second.is_null() || e.second->get_reference_count() <= 1);
    });
    std::erase_if(requested_, [&](const std::string& m) { return !needed.contains(m) && !models_.contains(m); });
    ++selections_;
    select_usec_ = std::max(select_usec_, now_usec() - started);
}

bool SkydotLargeRefs::drawn_or_skipped(std::uint32_t index) {
    if (shown_.contains(index) || info(index).model.empty()) {
        return true;
    }
    const auto* ref = world_->data().large_ref(world_id_, index);
    return ref == nullptr || built(ref->cell_x(), ref->cell_y());
}

void SkydotLargeRefs::update_marks(std::int32_t cx, std::int32_t cy) {
    std::set<std::pair<std::int32_t, std::int32_t>> now;
    std::vector<std::uint32_t> list;
    const auto radius = static_cast<std::int32_t>(radius_);
    for (std::int32_t y = cy - radius; y <= cy + radius; ++y) {
        for (std::int32_t x = cx - radius; x <= cx + radius; ++x) {
            list.clear();
            world_->data().large_cell_refs(world_id_, x, y, list);
            if (std::ranges::all_of(list, [this](std::uint32_t index) { return drawn_or_skipped(index); })) {
                now.emplace(x, y);
            }
        }
    }
    if (lod_ != nullptr) {
        for (const auto& c : marked_) {
            if (!now.contains(c)) {
                lod_->set_cell_large_refs(c.first, c.second, false);
            }
        }
        for (const auto& c : now) {
            if (!marked_.contains(c)) {
                lod_->set_cell_large_refs(c.first, c.second, true);
            }
        }
    }
    marked_ = std::move(now);
}

void SkydotLargeRefs::no_shadows(godot::Node* node) {
    if (auto* geometry = godot::Object::cast_to<godot::GeometryInstance3D>(node)) {
        geometry->set_cast_shadows_setting(godot::GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
    }
    for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
        no_shadows(node->get_child(i));
    }
}

std::int64_t SkydotLargeRefs::update(const Vector3& camera, std::int64_t budget_usec) {
    if (world_.is_null()) {
        return 0;
    }
    const auto game = SkydotWorld::godot_to_skyrim(camera);
    const auto cx = static_cast<std::int32_t>(std::floor(static_cast<double>(game.x) / k_cell_units));
    const auto cy = static_cast<std::int32_t>(std::floor(static_cast<double>(game.y) / k_cell_units));
    if (!placed_ || cx != cell_x_ || cy != cell_y_) {
        placed_ = true;
        cell_x_ = cx;
        cell_y_ = cy;
        dirty_ = true;
    }
    if (dirty_) {
        select(cx, cy, static_cast<double>(game.x), static_cast<double>(game.y));
        dirty_ = false;
        marks_dirty_ = true;
    }

    for (auto it = shown_.begin(); it != shown_.end();) {
        if (wanted_.contains(it->first)) {
            ++it;
            continue;
        }
        it->second->queue_free();
        it = shown_.erase(it);
        marks_dirty_ = true;
    }

    const auto started = now_usec();
    const auto assets = pack_->assets();
    pending_ = 0;
    for (const std::uint32_t index : candidates_) {
        if (shown_.contains(index)) {
            continue;
        }
        const std::string& model = info(index).model;
        if (model.empty()) {
            continue; // failed meanwhile
        }
        auto held = models_.find(model);
        if (held == models_.end()) {
            if (assets != nullptr && assets->request(model) == AssetCache::Status::loading) {
                ++pending_;
                continue;
            }
            held = models_.emplace(model, assets != nullptr ? assets->get(model) : Ref<godot::Resource>()).first;
        }
        if (held->second.is_null()) {
            infos_[index].model.clear();
            infos_[index].reason = "failed";
            ++skipped_["failed"];
            marks_dirty_ = true;
            continue;
        }
        if (now_usec() - started > budget_usec) {
            ++pending_;
            continue;
        }
        godot::Node3D* node = world_->build_large_ref(*world_->data().large_ref(world_id_, index));
        if (node == nullptr) {
            infos_[index].model.clear();
            infos_[index].reason = "failed";
            ++skipped_["failed"];
            marks_dirty_ = true;
            continue;
        }
        no_shadows(node);
        add_child(node);
        shown_.emplace(index, node);
        marks_dirty_ = true;
    }

    if (marks_dirty_) {
        update_marks(cx, cy);
        marks_dirty_ = false;
    }
    return pending_;
}

void SkydotLargeRefs::clear() {
    for (const auto& [index, node] : shown_) {
        node->queue_free();
    }
    shown_.clear();
    candidates_.clear();
    wanted_.clear();
    models_.clear();
    requested_.clear();
    if (lod_ != nullptr) {
        for (const auto& c : marked_) {
            lod_->set_cell_large_refs(c.first, c.second, false);
        }
    }
    marked_.clear();
    built_cells_.clear();
    dirty_ = true;
    marks_dirty_ = true;
}

Dictionary SkydotLargeRefs::get_stats() const {
    Dictionary out;
    out["wanted"] = static_cast<std::int64_t>(candidates_.size());
    out["shown"] = static_cast<std::int64_t>(shown_.size());
    out["pending"] = pending_;
    out["marked_cells"] = static_cast<std::int64_t>(marked_.size());
    out["selections"] = selections_;
    out["select_usec"] = select_usec_;
    out["models"] = static_cast<std::int64_t>(models_.size());
    Dictionary skipped;
    for (const auto& [reason, count] : skipped_) {
        skipped[String::utf8(reason.c_str())] = count;
    }
    out["skipped"] = skipped;
    return out;
}

} // namespace skydot
