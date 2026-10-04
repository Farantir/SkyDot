// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/coordinates.hpp"

#include "skydot_formats/units.hpp"

#include <godot_cpp/variant/basis.hpp>

#include <numbers>

using godot::Basis;
using godot::Transform3D;
using godot::Vector3;

namespace skydot {

namespace {

/// Z-up to Y-up: -90 degrees about X, as the converter's mesh writer does.
const Basis& axis_conversion() {
    static const Basis basis(Vector3(1, 0, 0), -std::numbers::pi_v<godot::real_t> / 2);
    return basis;
}

} // namespace

Vector3 skyrim_position(const Vector3& position) {
    return axis_conversion().xform(position * static_cast<godot::real_t>(formats::k_metres_per_unit));
}

Vector3 godot_to_skyrim(const Vector3& position) {
    return axis_conversion().transposed().xform(position) /
           static_cast<godot::real_t>(formats::k_metres_per_unit);
}

Transform3D skyrim_transform(const Vector3& position, const Vector3& rotation, double scale) {
    // Skyrim rotations are clockwise (negative in a right-handed frame) and
    // applied Z first, then Y, then X; see docs/coordinates.md.
    const Basis rx(Vector3(1, 0, 0), -rotation.x);
    const Basis ry(Vector3(0, 1, 0), -rotation.y);
    const Basis rz(Vector3(0, 0, 1), -rotation.z);
    const Basis skyrim = rx * ry * rz;
    const Basis& c = axis_conversion();
    Basis basis = c * skyrim * c.transposed();
    basis.scale(Vector3(1, 1, 1) * static_cast<godot::real_t>(scale));
    return Transform3D(basis, skyrim_position(position));
}

} // namespace skydot
