// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pieces of an actor using furniture that need no Godot: where a furniture marker puts the
// actor, from which side it is entered, and the actor's behaviour graph run with the root motion of
// its clips integrated into a body pose. `SkydotAi` (ai_furniture.cpp) drives the use itself.
//
// - A marker's pose: the furniture reference's position plus the marker's offset turned by the
//   reference's rotation (heights are the reference's: the animation poses the body on the seat),
//   facing the reference's rotation plus the marker's heading. Rotations are Skyrim's, clockwise
//   about Z from +Y.
// - An entry is one of the marker's allowed entry bits (0x1 front, 0x2 behind, 0x4 right, 0x8 left,
//   0x10 up), the one nearest the direction the actor comes from, in the frame of the seated body.
// - The root motion of the clips the graph plays, scaled by the actor's size, moves the body from
//   where the use starts; the same run, started at the origin, gives the displacement of a whole
//   enter, from which the point to walk to follows.
#pragma once

#include "actors/actor_graph.hpp"
#include "data/furniture_marker.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace skydot::ai {

/// A body in Skyrim space: position in game units, `yaw` the Skyrim rotation about Z (radians,
/// clockwise from +Y).
struct Pose {
    double x{};
    double y{};
    double z{};
    double yaw{};
};

/// `right`/`forward` in a frame facing `yaw`, as a vector in the world (x, y).
void turn(double yaw, double right, double forward, double& x, double& y);

/// Where marker `marker` of a furniture standing at `furniture` puts the body.
Pose marker_pose(const Pose& furniture, const FurnitureMarker& marker);

/// The entry bit of `marker` (placed at `at`) to come from `(from_x, from_y)`: of the entries it
/// allows, the one whose side of the seated body that point is nearest to; an up entry only if it
/// is the only one. 0 when the marker allows none.
std::uint16_t choose_entry(const FurnitureMarker& marker, const Pose& at, double from_x, double from_y);

/// The state of a furniture user as GetSitting and GetSleeping report it.
enum class UseState : std::int8_t { none = 0, entering = 2, in = 3, leaving = 4 };

/// An actor's behaviour graph with the root motion of the clip playing integrated into a pose.
class GraphRun {
public:
    GraphRun(std::shared_ptr<const ActorGraphDef> def, const std::function<float(const std::string&)>& duration,
             double scale);

    [[nodiscard]] ActorGraph& graph() { return graph_; }
    [[nodiscard]] const ActorGraph& graph() const { return graph_; }
    [[nodiscard]] const Pose& pose() const { return pose_; }
    /// The body is here now: the clip playing starts its motion from here.
    void set_pose(const Pose& pose);

    /// Each returns the events the graph raised (and, for `enter` and `send`, those it raised on the way).
    std::vector<std::string> enter(std::string_view event);
    std::vector<std::string> send(std::string_view event);
    std::vector<std::string> advance(double seconds);

    /// The graph has raised the event (any case) since `enter`.
    [[nodiscard]] bool saw(std::string_view event) const;
    /// The actor has sat or lain down: the graph raised IdleChairSitting or IdleBedSleeping.
    [[nodiscard]] bool settled() const;
    [[nodiscard]] bool in_furniture() const { return graph_.in_furniture(); }

private:
    void integrate();
    void note(const std::vector<std::string>& events);

    ActorGraph graph_;
    double scale_;
    Pose pose_;
    Pose base_;               ///< The pose the clip instance playing started from.
    RootMotion ref_;          ///< The clip's motion at that moment.
    bool valid_{false};       ///< A clip with motion is being followed.
    std::string clip_;
    std::string file_;
    float last_time_{};
    std::vector<std::string> seen_; ///< Lower case.
};

/// The displacement (at scale 1, from the origin facing +Y) of the enter that `event` starts, until the
/// graph has sat or lain the actor down; false if the graph does not get there.
bool enter_travel(const std::shared_ptr<const ActorGraphDef>& def, const std::function<float(const std::string&)>& duration,
                  std::string_view event, Pose& travel);

/// Where to start an enter that ends at `end` facing `end.yaw`, when it displaces the body by `travel` (at
/// scale 1) in its own frame (`travel.yaw` is the turn it makes): the actor starts facing `end.yaw` minus the turn.
Pose enter_start(const Pose& end, const Pose& travel, double scale);

} // namespace skydot::ai
