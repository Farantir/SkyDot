// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotWorld: doors, scripts and the other data activation needs, and
// picking the reference the player looks at.
#include "world/refs.hpp"
#include "world/collision.hpp"
#include "world/fb_search.hpp"
#include "world/world.hpp"

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

String type_name(std::uint32_t type) {
    const char chars[5] = {static_cast<char>(type & 0xFF), static_cast<char>((type >> 8) & 0xFF),
                           static_cast<char>((type >> 16) & 0xFF),
                           static_cast<char>((type >> 24) & 0xFF), 0};
    return String(chars);
}

String to_godot(const flatbuffers::String* s) {
    return s == nullptr ? String() : String::utf8(s->c_str(), static_cast<int>(s->size()));
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

/// Elements of a struct vector sorted by `ref()` that belong to `ref`: for the
/// vectors a reference can have several entries in.
template <typename T>
std::vector<const T*> by_ref(const flatbuffers::Vector<const T*>* list, std::uint32_t ref) {
    std::vector<const T*> out;
    if (list == nullptr) {
        return out;
    }
    for (auto i = first_at_least(list, ref, [](const T* e) { return e->ref(); });
         i < list->size() && list->Get(i)->ref() == ref; ++i) {
        out.push_back(list->Get(i));
    }
    return out;
}

const wfb::RefScripts* ref_scripts(const wfb::Cell& cell, std::uint32_t ref) {
    return lookup(cell.scripts(), ref);
}

const wfb::Ref* find_ref(const wfb::Cell& cell, std::uint32_t id) {
    return lookup(cell.refs(), id);
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

bool SkydotWorld::activatable(const wfb::Base* base, std::uint32_t cell, std::uint32_t ref) const {
    if (base != nullptr && (usable_type(base->type()) ||
                            (base->scripts() != nullptr && base->scripts()->size() != 0))) {
        return true;
    }
    if (doors_.contains(ref)) {
        return true;
    }
    const auto* c = cell_ptr(cell);
    return c != nullptr && ref_scripts(*c, ref) != nullptr;
}

const wfb::DoorLink* SkydotWorld::door_ptr(std::uint32_t ref) const {
    const auto it = doors_.find(ref);
    return it != doors_.end() ? it->second.second : nullptr;
}

Dictionary SkydotWorld::get_door(std::int64_t ref) const {
    Dictionary out;
    const auto it = doors_.find(static_cast<std::uint32_t>(ref));
    if (it == doors_.end()) {
        return out;
    }
    const auto& [cell, link] = it->second;
    out["ref"] = static_cast<std::int64_t>(link->ref());
    out["cell"] = static_cast<std::int64_t>(cell->id());
    out["destination"] = static_cast<std::int64_t>(link->destination());
    const auto dest = doors_.find(link->destination());
    const wfb::Cell* dest_cell = dest != doors_.end() ? dest->second.first : nullptr;
    out["destination_cell"] = static_cast<std::int64_t>(dest_cell != nullptr ? dest_cell->id() : 0);
    out["destination_world"] =
        static_cast<std::int64_t>(dest_cell != nullptr ? dest_cell->world() : 0);
    out["destination_interior"] =
        dest_cell != nullptr && formats::has_flag(dest_cell->flags(), wfb::CellFlags::interior);
    const auto& p = link->position();
    const auto& r = link->rotation();
    out["arrival"] = skyrim_transform(Vector3(p.x(), p.y(), p.z()), Vector3(r.x(), r.y(), r.z()), 1.0);
    return out;
}

Dictionary SkydotWorld::get_ref_info(std::int64_t cell_id, std::int64_t ref_id) const {
    Dictionary out;
    const auto* cell = cell_ptr(cell_id);
    const auto id = static_cast<std::uint32_t>(ref_id);
    const auto* ref = cell != nullptr ? find_ref(*cell, id) : nullptr;
    if (ref == nullptr) {
        return out;
    }
    const auto* base = base_ptr(ref->base());
    out["id"] = static_cast<std::int64_t>(id);
    out["cell"] = static_cast<std::int64_t>(cell->id());
    out["base"] = static_cast<std::int64_t>(ref->base());
    out["type"] = base != nullptr ? type_name(base->type()) : String();
    out["editor_id"] = base != nullptr ? to_godot(base->editor_id()) : String();
    out["activatable"] = activatable(base, cell->id(), id);
    out["parent_activate_only"] =
        formats::has_flag(ref->flags(), wfb::RefFlags::parent_activate_only);
    out["disabled"] = initially_disabled(*ref);
    out["enable_parent"] = static_cast<std::int64_t>(ref->enable_parent());
    out["enable_opposite"] = formats::has_flag(ref->flags(), wfb::RefFlags::enable_opposite);
    out["position"] = Vector3(ref->position().x(), ref->position().y(), ref->position().z());
    out["rotation"] = Vector3(ref->rotation().x(), ref->rotation().y(), ref->rotation().z());
    out["scale"] = ref->scale();
    const auto primitives = by_ref(cell->primitives(), id);
    if (primitives.empty()) {
        out["primitive"] = Variant();
    } else {
        Dictionary primitive;
        const auto& b = primitives.front()->bounds();
        primitive["bounds"] = Vector3(b.x(), b.y(), b.z());
        primitive["type"] = static_cast<std::int64_t>(primitives.front()->type());
        out["primitive"] = primitive;
    }

    if (const auto* locked = lookup(cell->locks(), id)) {
        Dictionary lock;
        lock["level"] = static_cast<std::int64_t>(locked->level());
        lock["key"] = static_cast<std::int64_t>(locked->key());
        out["lock"] = lock;
    } else {
        out["lock"] = Variant();
    }
    const Dictionary door = get_door(id);
    out["door"] = door.is_empty() ? Variant() : Variant(door);

    Array links;
    for (const auto* link : by_ref(cell->links(), id)) {
        Dictionary entry;
        entry["keyword"] = static_cast<std::int64_t>(link->keyword());
        entry["target"] = static_cast<std::int64_t>(link->target());
        links.push_back(entry);
    }
    out["links"] = links;
    Array parents;
    for (const auto* parent : by_ref(cell->activate_parents(), id)) {
        Dictionary entry;
        entry["parent"] = static_cast<std::int64_t>(parent->parent());
        entry["delay"] = static_cast<double>(parent->delay());
        parents.push_back(entry);
    }
    out["activate_parents"] = parents;

    Array scripts = script_list(base != nullptr ? base->scripts() : nullptr, false);
    if (const auto* own = ref_scripts(*cell, id)) {
        scripts.append_array(script_list(own->scripts(), true));
    }
    out["scripts"] = scripts;
    return out;
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

Dictionary SkydotWorld::pick_ref(godot::Node* root, const Vector3& from, const Vector3& to) const {
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

std::int64_t SkydotWorld::get_ref_cell(std::int64_t ref) const {
    const auto* cells = root_ != nullptr ? root_->cells() : nullptr;
    if (cells == nullptr) {
        return 0;
    }
    if (ref_cells_.empty()) {
        for (const auto* cell : *cells) {
            if (const auto* refs = cell->refs()) {
                for (const auto* r : *refs) {
                    ref_cells_.emplace_back(r->id(), cell->id());
                }
            }
        }
        std::ranges::sort(ref_cells_);
    }
    const auto id = static_cast<std::uint32_t>(ref);
    const auto it = std::lower_bound(ref_cells_.begin(), ref_cells_.end(),
                                     std::pair<std::uint32_t, std::uint32_t>{id, 0});
    return it != ref_cells_.end() && it->first == id ? it->second : 0;
}

godot::PackedInt64Array SkydotWorld::get_enable_children(std::int64_t ref) const {
    godot::PackedInt64Array out;
    const auto [begin, end] = enable_children_.equal_range(static_cast<std::uint32_t>(ref));
    std::vector<std::uint32_t> children;
    for (auto it = begin; it != end; ++it) {
        children.push_back(it->second);
    }
    std::ranges::sort(children);
    for (const auto child : children) {
        out.push_back(child);
    }
    return out;
}

Array SkydotWorld::get_activate_children(std::int64_t ref) const {
    Array out;
    const auto [begin, end] = activate_children_.equal_range(static_cast<std::uint32_t>(ref));
    std::vector<std::tuple<std::uint32_t, std::uint32_t, float>> children;
    for (auto it = begin; it != end; ++it) {
        children.push_back(it->second);
    }
    std::ranges::sort(children); // the multimap's order is unspecified
    for (const auto& [child, cell, delay] : children) {
        Dictionary entry;
        entry["ref"] = static_cast<std::int64_t>(child);
        entry["cell"] = static_cast<std::int64_t>(cell);
        entry["delay"] = static_cast<double>(delay);
        out.push_back(entry);
    }
    return out;
}

godot::PackedInt64Array SkydotWorld::get_scripted_refs(std::int64_t cell_id) const {
    godot::PackedInt64Array out;
    const auto* cell = cell_ptr(cell_id);
    if (cell == nullptr || cell->refs() == nullptr) {
        return out;
    }
    for (const auto* ref : *cell->refs()) {
        const auto* base = base_ptr(ref->base());
        const bool base_scripted = base != nullptr && base->scripts() != nullptr && base->scripts()->size() != 0;
        if (base_scripted || ref_scripts(*cell, ref->id()) != nullptr) {
            out.push_back(ref->id());
        }
    }
    return out;
}

} // namespace skydot
