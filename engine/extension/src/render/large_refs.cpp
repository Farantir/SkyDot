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
#include <string_view>
#include <unordered_set>

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

SkydotLargeRefs::Info& SkydotLargeRefs::info(std::uint32_t index) {
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
        out.cell_x = ref->cell_x();
        out.cell_y = ref->cell_y();
        out.x = static_cast<double>(ref->position().x());
        out.y = static_cast<double>(ref->position().y());
    } else {
        out.reason = "no base";
    }
    if (out.model.empty()) {
        ++skipped_[out.reason != nullptr ? out.reason : "no model"];
    }
    return infos_.emplace(index, std::move(out)).first->second;
}

const std::vector<SkydotLargeRefs::Info*>& SkydotLargeRefs::list(std::int32_t x, std::int32_t y) {
    const auto k = key(x, y);
    const auto found = lists_.find(k);
    if (found != lists_.end()) {
        return found->second;
    }
    std::vector<std::uint32_t> indices;
    world_->data().large_cell_refs(world_id_, x, y, indices);
    std::vector<Info*> infos;
    infos.reserve(indices.size());
    for (const std::uint32_t index : indices) {
        infos.push_back(&info(index));
    }
    return lists_.emplace(k, std::move(infos)).first->second;
}

void SkydotLargeRefs::select(std::int32_t cx, std::int32_t cy, double x, double y) {
    const auto started = now_usec();
    const auto chosen = large_refs::select(world_->data(), world_id_, cx, cy, static_cast<std::int32_t>(radius_),
                                           [this](std::int32_t gx, std::int32_t gy) { return built(gx, gy); });
    ++generation_;
    struct Ranked {
        double distance;
        std::uint32_t index;
        Info* info;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(chosen.size());
    for (const std::uint32_t index : chosen) {
        Info& i = info(index);
        if (i.model.empty()) {
            continue;
        }
        i.wanted = generation_;
        const double dx = i.x - x;
        const double dy = i.y - y;
        ranked.push_back({dx * dx + dy * dy, index, &i});
    }
    std::ranges::sort(ranked, {}, &Ranked::distance);
    candidates_.clear();
    candidates_.reserve(ranked.size());
    for (const Ranked& r : ranked) {
        candidates_.emplace_back(r.index, r.info);
    }
    // Models load on the asset cache's threads, all asked for at once (the
    // cache skips what it has or has queued).
    if (const auto assets = pack_->assets()) {
        for (const auto& [index, i] : candidates_) {
            if (!models_.contains(i->model)) {
                assets->request(i->model);
            }
        }
    }
    // What nothing wants any more lets go of its model, now and then.
    if (models_.size() > candidates_.size() + 64) {
        std::unordered_set<std::string_view> needed;
        for (const auto& [index, i] : candidates_) {
            needed.insert(i->model);
        }
        std::erase_if(models_, [&](const auto& e) {
            return !needed.contains(e.first) && (e.second.is_null() || e.second->get_reference_count() <= 1);
        });
    }
    ++selections_;
    const auto took = now_usec() - started;
    select_usec_ = std::max(select_usec_, took);
    select_total_ += took;
}

void SkydotLargeRefs::update_marks(std::int32_t cx, std::int32_t cy) {
    std::set<std::pair<std::int32_t, std::int32_t>> now;
    const auto radius = static_cast<std::int32_t>(radius_);
    for (std::int32_t y = cy - radius; y <= cy + radius; ++y) {
        for (std::int32_t x = cx - radius; x <= cx + radius; ++x) {
            const auto& infos = list(x, y);
            if (std::ranges::all_of(infos, [this](const Info* i) { return settled(*i); })) {
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

    const auto dropping = now_usec();
    // Those a built cell now draws go at once (else two are drawn); those out
    // of range go a few at a time, since freeing hundreds in a frame stalls it.
    constexpr int k_far_drops_per_update = 24;
    int far_drops = 0;
    std::int64_t kept = 0;
    for (auto it = shown_.begin(); it != shown_.end();) {
        if (it->second->wanted == generation_) {
            ++it;
            continue;
        }
        if (!built(it->second->cell_x, it->second->cell_y) && ++far_drops > k_far_drops_per_update) {
            ++kept;
            ++it;
            continue;
        }
        it->second->node->queue_free();
        it->second->node = nullptr;
        it = shown_.erase(it);
        marks_dirty_ = true;
    }
    drop_usec_ = std::max(drop_usec_, now_usec() - dropping);

    const auto started = now_usec();
    const auto assets = pack_->assets();
    pending_ = kept; // still to drop
    for (const auto& [index, i] : candidates_) {
        if (i->node != nullptr) {
            continue;
        }
        if (i->model.empty()) {
            continue; // failed meanwhile
        }
        auto held = models_.find(i->model);
        if (held == models_.end()) {
            if (assets != nullptr && assets->request(i->model) == AssetCache::Status::loading) {
                ++pending_;
                continue;
            }
            held = models_.emplace(i->model, assets != nullptr ? assets->get(i->model) : Ref<godot::Resource>()).first;
        }
        godot::Node3D* node = nullptr;
        if (held->second.is_valid()) {
            if (now_usec() - started > budget_usec) {
                ++pending_;
                continue;
            }
            node = world_->build_large_ref(*world_->data().large_ref(world_id_, index));
        }
        if (node == nullptr) {
            i->model.clear();
            i->reason = "failed";
            ++skipped_["failed"];
            marks_dirty_ = true;
            continue;
        }
        no_shadows(node);
        add_child(node);
        i->node = node;
        shown_.emplace(index, i);
        marks_dirty_ = true;
    }

    build_usec_ = std::max(build_usec_, now_usec() - started);
    if (marks_dirty_) {
        const auto marking = now_usec();
        update_marks(cx, cy);
        marks_dirty_ = false;
        marks_usec_ = std::max(marks_usec_, now_usec() - marking);
        marks_total_ += now_usec() - marking;
        ++marks_runs_;
    }
    return pending_;
}

void SkydotLargeRefs::clear() {
    for (const auto& [index, i] : shown_) {
        i->node->queue_free();
        i->node = nullptr;
    }
    shown_.clear();
    candidates_.clear();
    ++generation_;
    models_.clear();
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
    out["select_avg"] = selections_ > 0 ? select_total_ / selections_ : 0;
    out["marks_avg"] = marks_runs_ > 0 ? marks_total_ / marks_runs_ : 0;
    out["marks_runs"] = marks_runs_;
    out["drop_usec"] = drop_usec_;
    out["build_usec"] = build_usec_;
    out["marks_usec"] = marks_usec_;
    out["models"] = static_cast<std::int64_t>(models_.size());
    Dictionary skipped;
    for (const auto& [reason, count] : skipped_) {
        skipped[String::utf8(reason.c_str())] = count;
    }
    out["skipped"] = skipped;
    return out;
}

} // namespace skydot
