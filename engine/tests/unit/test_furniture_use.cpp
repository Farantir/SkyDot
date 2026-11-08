// SPDX-License-Identifier: GPL-3.0-or-later
//
// Furniture use without Godot (ai/furniture_use.hpp): where a marker puts the body, the entry an
// actor comes by, and the root motion of a graph run integrated into a pose.
#include "ai/furniture_use.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace skydot;
using Catch::Approx;

namespace {
constexpr double k_pi = std::numbers::pi;
}

TEST_CASE("a marker's pose turns its offset with the reference and keeps the reference's height", "[furniture_use]") {
    FurnitureMarker marker;
    marker.offset = {0.0F, -11.18F, 37.09F};
    // The bed of Alvor's house: rotation 270 degrees (clockwise from +Y), marker 11 units behind its middle.
    const ai::Pose bed{-232.35, -184.05, 240.0, 1.5 * k_pi};
    const ai::Pose at = ai::marker_pose(bed, marker);
    CHECK(at.x == Approx(-221.17).margin(0.01));
    CHECK(at.y == Approx(-184.05).margin(0.01));
    CHECK(at.z == Approx(240.0)); // the marker's height is the animation's business
    CHECK(at.yaw == Approx(1.5 * k_pi));
    marker.heading = 0.5F;
    CHECK(ai::marker_pose(bed, marker).yaw == Approx(1.5 * k_pi + 0.5));
}

TEST_CASE("the entry is the allowed side the actor is nearest to, in the frame of the seated body", "[furniture_use]") {
    FurnitureMarker chair;
    chair.entry_points = 0x1 | 0x4 | 0x8; // front, right, left
    // The chair faces north (yaw 0): front is +y, right +x.
    const ai::Pose at{0.0, 0.0, 0.0, 0.0};
    CHECK(ai::choose_entry(chair, at, 50.0, 3.0) == 0x4);
    CHECK(ai::choose_entry(chair, at, -50.0, 3.0) == 0x8);
    CHECK(ai::choose_entry(chair, at, 3.0, 60.0) == 0x1);
    // Behind is not allowed: the nearest allowed side wins.
    CHECK(ai::choose_entry(chair, at, 5.0, -60.0) == 0x4);
    // Turned a quarter turn clockwise (facing east): the front is +x, the right -y.
    const ai::Pose east{0.0, 0.0, 0.0, 0.5 * k_pi};
    CHECK(ai::choose_entry(chair, east, 60.0, 0.0) == 0x1);
    CHECK(ai::choose_entry(chair, east, 0.0, -60.0) == 0x4);
    // At the marker itself the first allowed side in front, behind, right, left order is taken.
    CHECK(ai::choose_entry(chair, at, 0.0, 0.0) == 0x1);
    // Only an up entry (the spider's shaft).
    FurnitureMarker shaft;
    shaft.entry_points = 0x10;
    CHECK(ai::choose_entry(shaft, at, 10.0, 10.0) == 0x10);
    CHECK(ai::choose_entry(FurnitureMarker{}, at, 10.0, 10.0) == 0);
}

TEST_CASE("the start of an enter is where its displacement leads to the marker", "[furniture_use]") {
    // An enter that moves the body 50 forward and turns it 90 degrees counter-clockwise... as a clockwise
    // yaw change of -pi/2: it ends facing end.yaw, so it starts facing end.yaw + pi/2.
    const ai::Pose end{100.0, 200.0, 10.0, 0.0};
    const ai::Pose travel{0.0, 50.0, 3.0, -0.5 * k_pi};
    const ai::Pose start = ai::enter_start(end, travel, 2.0);
    CHECK(start.yaw == Approx(0.5 * k_pi));
    // Facing east, 100 forward is 100 east: the start is 100 west of the marker.
    CHECK(start.x == Approx(0.0).margin(1e-6));
    CHECK(start.y == Approx(200.0).margin(1e-6));
    CHECK(start.z == Approx(4.0));
}
