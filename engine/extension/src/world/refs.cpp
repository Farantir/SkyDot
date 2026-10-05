// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a reference does beyond being placed: its scripts as Dictionaries,
// the metadata that marks a placed model, whether activating it can do
// anything, and picking the reference the player looks at.
#include "world/refs.hpp"
#include "world/world_data.hpp"
#include "world/collision.hpp"
#include "world/coordinates.hpp"
#include "world/fb_search.hpp"
#include "world/text.hpp"

#include "skydot_formats/flags.hpp"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/string.hpp>

#include <algorithm>
#include <limits>
#include <vector>

using godot::AABB;
using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Transform3D;
using godot::Variant;
using godot::Vector3;

namespace wfb = bethconv::pack::wfb;

namespace skydot {

namespace {

constexpr std::uint32_t fourcc(const char (&tag)[5]) {
    return static_cast<std::uint32_t>(tag[0]) | (static_cast<std::uint32_t>(tag[1]) << 8) |
           (static_cast<std::uint32_t>(tag[2]) << 16) | (static_cast<std::uint32_t>(tag[3]) << 24);
}

/// Types the player can use without a script: doors, activators, containers,
/// furniture, flora and items.
bool usable_type(std::uint32_t type) {
    static constexpr std::uint32_t k_types[] = {
        fourcc("DOOR"), fourcc("ACTI"), fourcc("CONT"), fourcc("FURN"), fourcc("FLOR"),
        fourcc("TACT"), fourcc("MISC"), fourcc("WEAP"), fourcc("ARMO"), fourcc("BOOK"),
        fourcc("ALCH"), fourcc("INGR"), fourcc("KEYM"), fourcc("AMMO"), fourcc("SLGM"),
        fourcc("SCRL"),
    };
    return std::ranges::find(k_types, type) != std::end(k_types);
}

Variant object_value(const wfb::ScriptObject& object) {
    if (object.alias() < 0) {
        return static_cast<std::int64_t>(object.form());
    }
    Dictionary alias;
    alias["form"] = static_cast<std::int64_t>(object.form());
    alias["alias"] = static_cast<std::int64_t>(object.alias());
    return alias;
}

/// A property's value; see get_ref_info.
Variant property_value(const wfb::ScriptProperty& p) {
    Array values;
    const std::uint8_t type = p.type();
    const std::uint8_t scalar = type >= 11 ? static_cast<std::uint8_t>(type - 10) : type;
    switch (scalar) {
    case 1:
        if (p.objects() != nullptr) {
            for (const auto* o : *p.objects()) {
                values.push_back(object_value(*o));
            }
        }
        break;
    case 2:
        if (p.strings() != nullptr) {
            for (const auto* s : *p.strings()) {
                values.push_back(to_godot(s));
            }
        }
        break;
    case 3:
    case 5:
        if (p.ints() != nullptr) {
            for (const auto i : *p.ints()) {
                values.push_back(scalar == 5 ? Variant(i != 0) : Variant(static_cast<std::int64_t>(i)));
            }
        }
        break;
    case 4:
        if (p.floats() != nullptr) {
            for (const auto f : *p.floats()) {
                values.push_back(static_cast<double>(f));
            }
        }
        break;
    default:
        return Variant();
    }
    if (type >= 11) {
        return values;
    }
    return values.is_empty() ? Variant() : values[0];
}

void merge_bounds(const godot::Node* node, const Transform3D& to_root, AABB& out, bool& any) {
    for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
        const auto* child = godot::Object::cast_to<godot::Node3D>(node->get_child(i));
        if (child == nullptr) {
            continue;
        }
        const Transform3D xf = to_root * child->get_transform();
        if (const auto* mesh = godot::Object::cast_to<godot::MeshInstance3D>(child);
            mesh != nullptr && mesh->get_mesh().is_valid()) {
            const AABB box = xf.xform(mesh->get_mesh()->get_aabb());
            out = any ? out.merge(box) : box;
            any = true;
        }
        merge_bounds(child, xf, out, any);
    }
}

/// The node's transform in the space of the outermost ancestor, which is
/// global space when it is in the tree.
Transform3D world_transform(const godot::Node3D* node) {
    if (node->is_inside_tree()) {
        return node->get_global_transform();
    }
    Transform3D xf = node->get_transform();
    for (const auto* p = godot::Object::cast_to<godot::Node3D>(node->get_parent()); p != nullptr;
         p = godot::Object::cast_to<godot::Node3D>(p->get_parent())) {
        xf = p->get_transform() * xf;
    }
    return xf;
}

/// Where the segment a-b (0..1) first enters `box`, or a negative value.
double segment_hit(const AABB& box, const Vector3& a, const Vector3& b) {
    double t_min = 0.0;
    double t_max = 1.0;
    const Vector3 hi_corner = box.position + box.size;
    for (int axis = 0; axis < 3; ++axis) {
        const double start = static_cast<double>(a[axis]);
        const double d = static_cast<double>(b[axis]) - start;
        const double lo = static_cast<double>(box.position[axis]);
        const double hi = static_cast<double>(hi_corner[axis]);
        if (std::abs(d) < 1e-12) {
            if (start < lo || start > hi) {
                return -1.0;
            }
            continue;
        }
        double t0 = (lo - start) / d;
        double t1 = (hi - start) / d;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        t_min = std::max(t_min, t0);
        t_max = std::min(t_max, t1);
        if (t_min > t_max) {
            return -1.0;
        }
    }
    return t_min;
}

} // namespace

Array script_list(const ScriptVector* scripts, bool from_ref) {
    Array out;
    if (scripts == nullptr) {
        return out;
    }
    for (const auto* s : *scripts) {
        Dictionary script;
        script["name"] = to_godot(s->name());
        script["status"] = static_cast<std::int64_t>(s->status());
        script["removed"] = formats::has_flag(s->status(), wfb::ScriptStatus::removed);
        script["from_ref"] = from_ref;
        Dictionary properties;
        if (s->properties() != nullptr) {
            for (const auto* p : *s->properties()) {
                properties[to_godot(p->name())] = property_value(*p);
            }
        }
        script["properties"] = properties;
        out.push_back(script);
    }
    return out;
}

bool door_type(std::uint32_t type) { return type == fourcc("DOOR"); }

void tag_ref(godot::Node3D* node, std::uint32_t ref, std::uint32_t cell, bool activatable) {
    node->set_meta("skydot_ref", static_cast<std::int64_t>(ref));
    node->set_meta("skydot_cell", static_cast<std::int64_t>(cell));
    if (!activatable) {
        return;
    }
    AABB bounds;
    bool any = false;
    merge_bounds(node, Transform3D(), bounds, any);
    if (!any) {
        return;
    }
    node->set_meta("skydot_activatable", true);
    node->set_meta("skydot_bounds", bounds);
}

bool activatable(const WorldData& data, const wfb::Base* base, std::uint32_t cell, std::uint32_t ref) {
    if (base != nullptr && (usable_type(base->type()) ||
                            (base->scripts() != nullptr && base->scripts()->size() != 0))) {
        return true;
    }
    if (data.doors().contains(ref)) {
        return true;
    }
    const auto* c = data.cell_ptr(cell);
    return c != nullptr && ref_scripts(*c, ref) != nullptr;
}

namespace {

/// The reference node a physics body belongs to, or null.
godot::Node* ref_of(godot::Object* collider) {
    for (auto* node = godot::Object::cast_to<godot::Node>(collider); node != nullptr;
         node = node->get_parent()) {
        if (node->has_meta("skydot_ref")) {
            return node;
        }
    }
    return nullptr;
}

/// The first world, clutter or terrain body along the segment: the collider
/// and the distance. Null outside a scene tree.
std::pair<godot::Object*, double> first_body(godot::Node* root, const Vector3& from,
                                             const Vector3& to) {
    auto* spatial = godot::Object::cast_to<godot::Node3D>(root);
    if (spatial == nullptr || !spatial->is_inside_tree()) {
        return {nullptr, 0.0};
    }
    const godot::Ref<godot::World3D> world = spatial->get_world_3d();
    auto* space = world.is_valid() ? world->get_direct_space_state() : nullptr;
    if (space == nullptr) {
        return {nullptr, 0.0};
    }
    const auto query = godot::PhysicsRayQueryParameters3D::create(
        from, to, physics_layer::solid);
    const Dictionary hit = space->intersect_ray(query);
    if (hit.is_empty()) {
        return {nullptr, 0.0};
    }
    const Vector3 position = hit["position"];
    return {static_cast<godot::Object*>(hit["collider"]), static_cast<double>(from.distance_to(position))};
}

} // namespace

Dictionary pick_ref(godot::Node* root, const Vector3& from, const Vector3& to) {
    Dictionary out;
    if (root == nullptr) {
        return out;
    }
    // Where there is collision, the first body hit decides: its reference if
    // that is usable, else nothing behind it. Bounds still find what has no
    // collision of its own in front of it.
    const auto [collider, blocked_at] = first_body(root, from, to);
    if (collider != nullptr) {
        auto* node = ref_of(collider);
        auto* model = godot::Object::cast_to<godot::Node3D>(node);
        if (model != nullptr && model->is_visible_in_tree() && node->has_meta("skydot_activatable")) {
            out["ref"] = node->get_meta("skydot_ref");
            out["cell"] = node->get_meta("skydot_cell");
            out["node"] = node;
            out["distance"] = blocked_at;
            out["position"] = from + (to - from).normalized() * static_cast<godot::real_t>(blocked_at);
            return out;
        }
    }
    // Bounds are boxes around the model, so they start in front of its
    // surface; allow a little.
    constexpr double k_bounds_slack = 0.05; // metres
    double best = collider != nullptr ? blocked_at + k_bounds_slack
                                      : std::numeric_limits<double>::infinity();
    std::vector<godot::Node*> pending{root};
    while (!pending.empty()) {
        godot::Node* node = pending.back();
        pending.pop_back();
        for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
            godot::Node* child = node->get_child(i);
            if (!child->has_meta("skydot_ref")) {
                pending.push_back(child);
                continue;
            }
            auto* model = godot::Object::cast_to<godot::Node3D>(child);
            if (model == nullptr || !model->is_visible() || !child->has_meta("skydot_activatable")) {
                continue;
            }
            const AABB bounds = child->get_meta("skydot_bounds");
            const Transform3D xf = world_transform(model);
            const Transform3D inverse = xf.affine_inverse();
            const double t = segment_hit(bounds, inverse.xform(from), inverse.xform(to));
            if (t < 0.0) {
                continue;
            }
            const Vector3 hit = from + (to - from) * static_cast<godot::real_t>(t);
            const double distance = static_cast<double>(from.distance_to(hit));
            if (distance < best) {
                best = distance;
                out["ref"] = child->get_meta("skydot_ref");
                out["cell"] = child->get_meta("skydot_cell");
                out["node"] = child;
                out["distance"] = distance;
                out["position"] = hit;
            }
        }
    }
    return out;
}

} // namespace skydot
