// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/ragdoll.hpp"

#include "render/effect_asset.hpp"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace skydot {

using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Variant;
using godot::Vector3;

namespace {

bool numeric(const Variant& v) { return v.get_type() == Variant::INT || v.get_type() == Variant::FLOAT; }

double number(const Variant& v) { return numeric(v) ? static_cast<double>(v) : 0.0; }

/// The first `n` numbers of a JSON array (glTF extras), zero where missing.
template <std::size_t N>
std::array<double, N> numbers(const Variant& v) {
    std::array<double, N> out{};
    if (v.get_type() == Variant::ARRAY) {
        const Array a = v;
        for (std::int64_t i = 0; i < a.size() && static_cast<std::size_t>(i) < N; ++i) {
            out[static_cast<std::size_t>(i)] = number(a[i]);
        }
    }
    return out;
}

Vector3 vector3(const Variant& v) {
    const auto n = numbers<3>(v);
    return {static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2])};
}

/// {translation: [x, y, z], rotation: [x, y, z, w]}.
godot::Transform3D transform(const Variant& v) {
    if (v.get_type() != Variant::DICTIONARY) {
        return {};
    }
    const Dictionary d = v;
    const auto q = numbers<4>(d.get("rotation", Variant()));
    godot::Quaternion rotation(static_cast<float>(q[0]), static_cast<float>(q[1]), static_cast<float>(q[2]),
                               static_cast<float>(q[3]));
    rotation = rotation.length_squared() > 0.0F ? rotation.normalized() : godot::Quaternion();
    return {godot::Basis(rotation), vector3(d.get("translation", Variant()))};
}

/// Every key of `from` as it is, numbers as floats; the caller replaces the typed ones.
Dictionary copy(const Dictionary& from) {
    Dictionary out;
    const Array keys = from.keys();
    for (std::int64_t i = 0; i < keys.size(); ++i) {
        const Variant value = from[keys[i]];
        out[keys[i]] = numeric(value) ? Variant(number(value)) : value;
    }
    return out;
}

void to_int(Dictionary& d, const char* key) {
    if (d.has(key)) {
        d[key] = static_cast<std::int64_t>(number(d[key]));
    }
}

Dictionary shape_of(const Dictionary& from) {
    Dictionary out = copy(from);
    out["transform"] = transform(from.get("transform", Variant()));
    to_int(out, "havok_material");
    for (const char* key : {"point_a", "point_b", "half_extents"}) {
        if (from.has(key)) {
            out[key] = vector3(from[key]);
        }
    }
    if (from.get("vertices", Variant()).get_type() == Variant::ARRAY) {
        const Array flat = from["vertices"];
        godot::PackedVector3Array vertices;
        for (std::int64_t i = 0; i + 2 < flat.size(); i += 3) {
            vertices.push_back(Vector3(static_cast<float>(number(flat[i])), static_cast<float>(number(flat[i + 1])),
                                       static_cast<float>(number(flat[i + 2]))));
        }
        out["vertices"] = vertices;
    }
    return out;
}

Dictionary body_of(const Dictionary& from) {
    Dictionary out = copy(from);
    out["body_transform"] = transform(from.get("body_transform", Variant()));
    out["bone_bind"] = transform(from.get("bone_bind", Variant()));
    out["center_of_mass"] = vector3(from.get("center_of_mass", Variant()));
    const auto i = numbers<9>(from.get("inertia", Variant()));
    const auto f = [&](std::size_t k) { return static_cast<float>(i[k]); };
    out["inertia"] = godot::Basis(f(0), f(1), f(2), f(3), f(4), f(5), f(6), f(7), f(8));
    for (const char* key : {"node", "collision_flags", "motion_type", "deactivator_type", "solver_deactivation",
                            "quality_type"}) {
        to_int(out, key);
    }
    Array shapes;
    if (from.get("shapes", Variant()).get_type() == Variant::ARRAY) {
        const Array list = from["shapes"];
        for (std::int64_t k = 0; k < list.size(); ++k) {
            if (list[k].get_type() == Variant::DICTIONARY) {
                shapes.push_back(shape_of(list[k]));
            }
        }
    }
    out["shapes"] = shapes;
    // The Havok filter, flattened: nif.xml HavokFilter, part number in bits 0-4 of the flags.
    out.erase("filter");
    const Variant filter = from.get("filter", Variant());
    const Dictionary f_dict = filter.get_type() == Variant::DICTIONARY ? Dictionary(filter) : Dictionary();
    const auto flags = static_cast<std::int64_t>(number(f_dict.get("flags_and_part", Variant())));
    out["layer"] = static_cast<std::int64_t>(number(f_dict.get("layer", Variant())));
    out["flags_and_part"] = flags;
    out["part_number"] = flags & 0x1F;
    out["group"] = static_cast<std::int64_t>(number(f_dict.get("group", Variant())));
    godot::PackedInt32Array constraints;
    if (from.get("constraints", Variant()).get_type() == Variant::ARRAY) {
        const Array list = from["constraints"];
        for (std::int64_t k = 0; k < list.size(); ++k) {
            constraints.push_back(static_cast<std::int32_t>(number(list[k])));
        }
    }
    out["constraints"] = constraints;
    return out;
}

Dictionary frame_of(const Variant& from) {
    Dictionary out;
    if (from.get_type() != Variant::DICTIONARY) {
        out["pivot"] = Vector3();
        return out;
    }
    const Dictionary d = from;
    const Array keys = d.keys();
    for (std::int64_t i = 0; i < keys.size(); ++i) {
        out[keys[i]] = vector3(d[keys[i]]);
    }
    return out;
}

} // namespace

Dictionary model_ragdoll(const godot::Node* model) {
    const Variant extra = bethconv_model_extra(model, "ragdoll");
    if (extra.get_type() != Variant::DICTIONARY) {
        return {};
    }
    const Dictionary from = extra;
    Array bodies;
    if (from.get("bodies", Variant()).get_type() == Variant::ARRAY) {
        const Array list = from["bodies"];
        for (std::int64_t i = 0; i < list.size(); ++i) {
            if (list[i].get_type() == Variant::DICTIONARY) {
                bodies.push_back(body_of(list[i]));
            }
        }
    }
    if (bodies.is_empty()) {
        return {};
    }
    Array constraints;
    std::vector<std::int32_t> renumbered; // Old constraint index -> kept one, -1 if left out.
    if (from.get("constraints", Variant()).get_type() == Variant::ARRAY) {
        const Array list = from["constraints"];
        renumbered.assign(static_cast<std::size_t>(list.size()), -1);
        for (std::int64_t i = 0; i < list.size(); ++i) {
            if (list[i].get_type() != Variant::DICTIONARY) {
                continue;
            }
            const Dictionary c = list[i];
            const double a = number(c.get("body_a", Variant(-1)));
            const double b = number(c.get("body_b", Variant(-1)));
            if (a < 0 || b < 0 || a >= static_cast<double>(bodies.size()) || b >= static_cast<double>(bodies.size())) {
                continue;
            }
            Dictionary out = copy(c);
            to_int(out, "body_a");
            to_int(out, "body_b");
            to_int(out, "priority");
            out["in_a"] = frame_of(c.get("in_a", Variant()));
            out["in_b"] = frame_of(c.get("in_b", Variant()));
            if (c.get("motor", Variant()).get_type() == Variant::DICTIONARY) {
                out["motor"] = copy(c["motor"]);
            }
            renumbered[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(constraints.size());
            constraints.push_back(out);
        }
    }
    for (std::int64_t i = 0; i < bodies.size(); ++i) {
        Dictionary body = bodies[i];
        const godot::PackedInt32Array listed = body["constraints"];
        godot::PackedInt32Array kept;
        for (std::int64_t k = 0; k < listed.size(); ++k) {
            const std::int32_t old = listed[k];
            if (old >= 0 && static_cast<std::size_t>(old) < renumbered.size() &&
                renumbered[static_cast<std::size_t>(old)] >= 0) {
                kept.push_back(renumbered[static_cast<std::size_t>(old)]);
            }
        }
        body["constraints"] = kept;
    }
    Dictionary out;
    out["bodies"] = bodies;
    out["constraints"] = constraints;
    return out;
}

} // namespace skydot
