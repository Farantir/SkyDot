// SPDX-License-Identifier: GPL-3.0-or-later
#include "ai/furniture_use.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

namespace skydot::ai {
namespace {

/// Graph steps are short so that a clip that ends inside one loses little of its motion.
constexpr double k_step = 1.0 / 30.0;
/// A walked-in enter that has not seated the actor after this long is given up in `enter_travel`.
constexpr double k_enter_limit = 60.0;

std::string lowered(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

/// The pose after `m`, a clip's motion at some time, when the instance started at `base` with motion `ref`.
Pose moved(const Pose& base, const RootMotion& ref, const RootMotion& m, double scale) {
    Pose out = base;
    double wx = 0.0;
    double wy = 0.0;
    turn(base.yaw, static_cast<double>(m.x - ref.x) * scale, static_cast<double>(m.y - ref.y) * scale, wx, wy);
    out.x += wx;
    out.y += wy;
    out.z += static_cast<double>(m.z - ref.z) * scale;
    // The motion's turn is counter-clockwise; Skyrim's rotations are clockwise.
    out.yaw -= static_cast<double>(m.yaw - ref.yaw);
    return out;
}

} // namespace

void turn(double yaw, double right, double forward, double& x, double& y) {
    x = right * std::cos(yaw) + forward * std::sin(yaw);
    y = -right * std::sin(yaw) + forward * std::cos(yaw);
}

Pose marker_pose(const Pose& furniture, const FurnitureMarker& marker) {
    double dx = 0.0;
    double dy = 0.0;
    turn(furniture.yaw, static_cast<double>(marker.offset[0]), static_cast<double>(marker.offset[1]), dx, dy);
    return Pose{furniture.x + dx, furniture.y + dy, furniture.z, furniture.yaw + static_cast<double>(marker.heading)};
}

std::uint16_t choose_entry(const FurnitureMarker& marker, const Pose& at, double from_x, double from_y) {
    // The direction to the actor in the frame of the seated body: right and forward.
    const double dx = from_x - at.x;
    const double dy = from_y - at.y;
    const double right = dx * std::cos(at.yaw) - dy * std::sin(at.yaw);
    const double forward = dx * std::sin(at.yaw) + dy * std::cos(at.yaw);
    const double length = std::hypot(right, forward);
    struct Side {
        std::uint16_t bit;
        double right;
        double forward;
    };
    static constexpr std::array<Side, 4> sides{{{0x1, 0.0, 1.0}, {0x2, 0.0, -1.0}, {0x4, 1.0, 0.0}, {0x8, -1.0, 0.0}}};
    std::uint16_t best = 0;
    double best_score = -2.0;
    for (const Side& s : sides) {
        if ((marker.entry_points & s.bit) == 0) {
            continue;
        }
        const double score = length > 0.0 ? (right * s.right + forward * s.forward) / length : 0.0;
        if (score > best_score) {
            best_score = score;
            best = s.bit;
        }
    }
    if (best == 0 && (marker.entry_points & 0x10) != 0) {
        best = 0x10;
    }
    return best;
}

GraphRun::GraphRun(std::shared_ptr<const ActorGraphDef> def, const std::function<float(const std::string&)>& duration,
                   double scale)
    : graph_(std::move(def), duration), scale_(scale) {
    graph_.configure(false);
    graph_.start();
}

void GraphRun::set_pose(const Pose& pose) {
    pose_ = pose;
    valid_ = false;
}

void GraphRun::note(const std::vector<std::string>& events) {
    for (const std::string& e : events) {
        seen_.push_back(lowered(e));
    }
}

bool GraphRun::saw(std::string_view event) const {
    const std::string want = lowered(event);
    return std::ranges::find(seen_, want) != seen_.end();
}

bool GraphRun::settled() const { return saw("IdleChairSitting") || saw("IdleBedSleeping"); }

void GraphRun::integrate() {
    const auto now = graph_.playing();
    const auto motion = now ? graph_.def().root_motion(now->clip, now->time) : std::nullopt;
    if (!motion) {
        valid_ = false; // a state without motion (or clip): the body stays
        return;
    }
    const bool same = valid_ && now->clip == clip_ && now->file == file_;
    if (same && now->time + 1e-4F < last_time_) {
        // A looping clip started its next lap: finish this one at its end.
        if (const auto end = graph_.def().root_motion(now->clip, 1.0e9F)) {
            pose_ = moved(base_, ref_, *end, scale_);
        }
        base_ = pose_;
        ref_ = graph_.def().root_motion(now->clip, 0.0F).value_or(*motion);
    } else if (!same) {
        base_ = pose_;
        ref_ = *motion;
        clip_ = now->clip;
        file_ = now->file;
    }
    pose_ = moved(base_, ref_, *motion, scale_);
    last_time_ = now->time;
    valid_ = true;
}

std::vector<std::string> GraphRun::enter(std::string_view event) {
    seen_.clear();
    BehaviorOutput out = graph_.send_entering(event);
    integrate();
    note(out.events);
    return std::move(out.events);
}

std::vector<std::string> GraphRun::send(std::string_view event) {
    BehaviorOutput out = graph_.send(event);
    integrate();
    note(out.events);
    return std::move(out.events);
}

std::vector<std::string> GraphRun::advance(double seconds) {
    std::vector<std::string> events;
    for (double left = seconds; left > 0.0;) {
        const double step = std::min(left, k_step);
        left -= step;
        BehaviorOutput out = graph_.advance(static_cast<float>(step));
        integrate();
        note(out.events);
        events.insert(events.end(), out.events.begin(), out.events.end());
    }
    return events;
}

bool enter_travel(const std::shared_ptr<const ActorGraphDef>& def, const std::function<float(const std::string&)>& duration,
                  std::string_view event, Pose& travel) {
    GraphRun run(def, duration, 1.0);
    run.set_pose(Pose{});
    run.enter(event);
    for (double t = 0.0; !run.settled() && t < k_enter_limit; t += k_step) {
        run.advance(k_step);
    }
    travel = run.pose();
    return run.settled();
}

Pose enter_start(const Pose& end, const Pose& travel, double scale) {
    // The enter turns the body by travel.yaw (clockwise), so it starts facing end.yaw - travel.yaw; its
    // displacement is travel in that frame.
    Pose start;
    start.yaw = end.yaw - travel.yaw;
    double dx = 0.0;
    double dy = 0.0;
    turn(start.yaw, travel.x * scale, travel.y * scale, dx, dy);
    start.x = end.x - dx;
    start.y = end.y - dy;
    start.z = end.z - travel.z * scale;
    return start;
}

} // namespace skydot::ai
