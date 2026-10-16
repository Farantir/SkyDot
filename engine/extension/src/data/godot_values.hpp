// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small conversions between Godot's Variants and the plain types the engine
// works with, which several layers need: one definition of each.
#pragma once

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

namespace skydot {

/// A form id carried in a Variant (Godot has only 64-bit integers).
inline std::uint32_t form_of(const godot::Variant& v) {
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(v));
}

/// A vector from three doubles (a position or rotation in Skyrim space).
inline godot::Vector3 vec3_of(const double (&v)[3]) {
    return godot::Vector3(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]));
}

/// A vector from anything with x(), y(), z() (world.fb's Vec3f).
template <typename V>
inline godot::Vector3 vec3_of(const V& v) {
    return godot::Vector3(v.x(), v.y(), v.z());
}

/// `d[key]` as a vector if it is a three-element array of numbers; otherwise
/// `fallback`, and `*ok` (if given) set false. `*ok` is never set true.
inline godot::Vector3 vec3_from(const godot::Dictionary& d, const char* key,
                                godot::Vector3 fallback = godot::Vector3(), bool* ok = nullptr) {
    const godot::Variant v = d.get(key, godot::Variant());
    if (v.get_type() != godot::Variant::ARRAY || godot::Array(v).size() != 3) {
        if (ok != nullptr) {
            *ok = false;
        }
        return fallback;
    }
    const godot::Array a = v;
    return godot::Vector3(static_cast<float>(static_cast<double>(a[0])), static_cast<float>(static_cast<double>(a[1])),
                          static_cast<float>(static_cast<double>(a[2])));
}

} // namespace skydot
