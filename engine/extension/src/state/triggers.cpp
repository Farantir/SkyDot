// SPDX-License-Identifier: GPL-3.0-or-later
#include "state/triggers.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace skydot::vm {

namespace {

float length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

bool inside_box(const Vec3& local, const Vec3& half) {
    return std::abs(local.x) <= half.x && std::abs(local.y) <= half.y && std::abs(local.z) <= half.z;
}

/// Grows [lo, hi] by the world box of the local box |l_i| <= h_i seen through
/// `to_local` (l = M p + o). A singular matrix makes the box unbounded.
void grow_bounds(const Affine& a, const Vec3& h, bool& first, Vec3& lo, Vec3& hi, bool& unbounded) {
    double m[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            m[i][j] = static_cast<double>(a.m[i][j]);
        }
    }
    const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    if (!(std::abs(det) > 1e-12) || !std::isfinite(det)) {
        unbounded = true;
        return;
    }
    double inv[3][3];
    inv[0][0] = c00 / det;
    inv[1][0] = c01 / det;
    inv[2][0] = c02 / det;
    inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    const double o[3] = {static_cast<double>(a.origin.x), static_cast<double>(a.origin.y),
                         static_cast<double>(a.origin.z)};
    const double hh[3] = {static_cast<double>(h.x), static_cast<double>(h.y), static_cast<double>(h.z)};
    for (int i = 0; i < 3; ++i) {
        // p_i = sum_j inv[i][j] * (l_j - o_j)
        double centre = 0.0;
        double extent = 0.0;
        for (int j = 0; j < 3; ++j) {
            centre -= inv[i][j] * o[j];
            extent += std::abs(inv[i][j]) * hh[j];
        }
        // Slack for float rounding in contains().
        const double pad = 1e-3 + 1e-4 * (std::abs(centre) + extent);
        const auto low = static_cast<float>(centre - extent - pad);
        const auto high = static_cast<float>(centre + extent + pad);
        float* l = i == 0 ? &lo.x : i == 1 ? &lo.y : &lo.z;
        float* u = i == 0 ? &hi.x : i == 1 ? &hi.y : &hi.z;
        *l = first ? low : std::min(*l, low);
        *u = first ? high : std::max(*u, high);
    }
    first = false;
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

void Trigger::update_bounds() {
    bool first = true;
    bool unbounded = false;
    Vec3 lo;
    Vec3 hi;
    for (const Volume& v : volumes) {
        const float r = v.sphere ? v.half.x : 0.0F;
        grow_bounds(v.to_local, v.sphere ? Vec3{r, r, r} : v.half, first, lo, hi, unbounded);
    }
    if (primitive) {
        // local = axes * (p - origin), so as a to_local the offset is -axes * origin.
        Affine a = axes;
        a.origin = {0.0F, 0.0F, 0.0F};
        const Vec3 shift = a.xform(origin);
        a.origin = {-shift.x, -shift.y, -shift.z};
        const float r = type == k_sphere ? half.x : 0.0F;
        grow_bounds(a, type == k_sphere ? Vec3{r, r, r} : half, first, lo, hi, unbounded);
    }
    if (unbounded) {
        bounds_min = {-1e30F, -1e30F, -1e30F};
        bounds_max = {1e30F, 1e30F, 1e30F};
    } else if (first) { // nothing can be inside
        bounds_min = {1.0F, 1.0F, 1.0F};
        bounds_max = {-1.0F, -1.0F, -1.0F};
    } else {
        bounds_min = lo;
        bounds_max = hi;
    }
}

void Triggers::set_primitive(std::uint32_t ref, Trigger trigger) {
    if (const auto old = triggers_.find(ref); old != triggers_.end()) {
        trigger.volumes = std::move(old->second.volumes);
    }
    trigger.update_bounds();
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
    it->second.update_bounds();
}

void Triggers::move_actor(std::uint32_t actor, const Vec3& position,
                          const std::function<bool(std::uint32_t)>& notices,
                          const std::function<void(std::uint32_t, bool)>& crossed) {
    actors_[actor] = position;
    // The callbacks may forget triggers, so they run after the walk over the map.
    std::vector<std::pair<std::uint32_t, bool>> crossings;
    for (auto& [ref, t] : triggers_) {
        const bool in = t.in_bounds(position) && t.contains(position);
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
