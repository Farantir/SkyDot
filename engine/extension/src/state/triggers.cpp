// SPDX-License-Identifier: GPL-3.0-or-later
#include "state/triggers.hpp"

#include <cmath>
#include <vector>

namespace skydot::vm {

namespace {

float length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

bool inside_box(const Vec3& local, const Vec3& half) {
    return std::abs(local.x) <= half.x && std::abs(local.y) <= half.y && std::abs(local.z) <= half.z;
}

} // namespace

Vec3 Affine::xform(const Vec3& v) const {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + origin.x,
            m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + origin.y,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + origin.z};
}

bool Trigger::contains(const Vec3& position) const {
    for (const Volume& v : volumes) {
        const Vec3 local = v.to_local.xform(position);
        if (v.sphere ? length(local) <= v.half.x : inside_box(local, v.half)) {
            return true;
        }
    }
    if (!primitive) {
        return false;
    }
    const Vec3 local = axes.xform({position.x - origin.x, position.y - origin.y, position.z - origin.z});
    return type == k_sphere ? length(local) <= half.x : inside_box(local, half);
}

void Triggers::set_primitive(std::uint32_t ref, Trigger trigger) {
    if (const auto old = triggers_.find(ref); old != triggers_.end()) {
        trigger.volumes = std::move(old->second.volumes);
    }
    triggers_[ref] = std::move(trigger);
}

void Triggers::set_volumes(std::uint32_t ref, std::vector<Volume> volumes) {
    if (volumes.empty()) {
        return;
    }
    auto [it, fresh] = triggers_.try_emplace(ref);
    if (fresh) {
        it->second.primitive = false;
    }
    it->second.volumes = std::move(volumes);
}

void Triggers::move_actor(std::uint32_t actor, const Vec3& position,
                          const std::function<bool(std::uint32_t)>& notices,
                          const std::function<void(std::uint32_t, bool)>& crossed) {
    actors_[actor] = position;
    // The callbacks may forget triggers, so they run after the walk over the map.
    std::vector<std::pair<std::uint32_t, bool>> crossings;
    for (auto& [ref, t] : triggers_) {
        const bool in = t.contains(position);
        const bool was = t.inside.contains(actor);
        if (in == was) {
            continue;
        }
        // A disabled trigger notices no one.
        if (in && !notices(ref)) {
            continue;
        }
        if (in) {
            t.inside.insert(actor);
        } else {
            t.inside.erase(actor);
        }
        crossings.emplace_back(ref, in);
    }
    for (const auto& [ref, in] : crossings) {
        if (triggers_.contains(ref)) {
            crossed(ref, in);
        }
    }
}

namespace {
/// An actor inside a trigger: ref (u32).
constexpr std::size_t k_trigger_occupant_min_bytes = 4;
/// A trigger: ref (u32), occupant count (u32).
constexpr std::size_t k_trigger_min_bytes = 4 + 4;
/// A tracked actor: ref (u32), position (three f64).
constexpr std::size_t k_trigger_actor_min_bytes = 4 + 3 * 8;
} // namespace

void Triggers::write(save::ByteWriter& w) const {
    w.u32(static_cast<std::uint32_t>(triggers_.size()));
    for (const auto& [ref, t] : triggers_) {
        const auto start = w.out.size();
        w.u32(ref);
        w.u32(static_cast<std::uint32_t>(t.inside.size()));
        for (const auto a : t.inside) {
            const auto occupant_start = w.out.size();
            w.u32(a);
            w.check_record(occupant_start, k_trigger_occupant_min_bytes);
        }
        w.check_record(start, k_trigger_min_bytes);
    }
    w.u32(static_cast<std::uint32_t>(actors_.size()));
    for (const auto& [ref, position] : actors_) {
        const auto start = w.out.size();
        w.u32(ref);
        w.f64(static_cast<double>(position.x));
        w.f64(static_cast<double>(position.y));
        w.f64(static_cast<double>(position.z));
        w.check_record(start, k_trigger_actor_min_bytes);
    }
}

bool Triggers::read(save::ByteReader& r, Saved& out) {
    std::uint32_t n = 0;
    if (!r.count(n, k_trigger_min_bytes)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t ref = 0, inside = 0;
        if (!r.u32(ref) || !r.count(inside, k_trigger_occupant_min_bytes)) {
            return false;
        }
        auto& list = out.inside[ref];
        for (std::uint32_t k = 0; k < inside; ++k) {
            std::uint32_t actor = 0;
            if (!r.u32(actor)) {
                return false;
            }
            list.insert(actor);
        }
    }
    if (!r.count(n, k_trigger_actor_min_bytes)) {
        return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t ref = 0;
        double x = 0.0, y = 0.0, z = 0.0;
        if (!r.u32(ref) || !r.f64(x) || !r.f64(y) || !r.f64(z)) {
            return false;
        }
        out.actors[ref] = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    }
    return true;
}

void Triggers::restore(Saved&& saved) {
    for (auto& [ref, actors] : saved.inside) {
        if (const auto it = triggers_.find(ref); it != triggers_.end()) {
            it->second.inside = std::move(actors);
        }
    }
    actors_ = std::move(saved.actors);
}

} // namespace skydot::vm
