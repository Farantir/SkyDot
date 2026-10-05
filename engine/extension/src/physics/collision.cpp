// SPDX-License-Identifier: GPL-3.0-or-later
#include "physics/collision.hpp"

#include "render/effect_asset.hpp"

#include <godot_cpp/classes/animatable_body3d.hpp>
#include <godot_cpp/classes/bone_attachment3d.hpp>
#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/capsule_shape3d.hpp>
#include <godot_cpp/classes/cylinder_shape3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/concave_polygon_shape3d.hpp>
#include <godot_cpp/classes/convex_polygon_shape3d.hpp>
#include <godot_cpp/classes/physics_direct_body_state3d.hpp>
#include <godot_cpp/classes/physics_material.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/sphere_shape3d.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

using godot::Array;
using godot::Dictionary;
using godot::Ref;
using godot::Transform3D;
using godot::Variant;
using godot::Vector3;

namespace skydot {

namespace {

// Skyrim collision layers (SkyrimLayer in nif.xml) by what they mean for a
// walking player.
enum class Role { ignored, solid, movable };

Role role_of(std::int64_t layer) {
    switch (layer) {
    case 0:  // unidentified
    case 1:  // static
    case 2:  // animated static
    case 3:  // transparent
    case 9:  // trees
    case 13: // terrain
    case 14: // trap
    case 17: // ground
    case 26: // transparent small
    case 27: // invisible wall
    case 28: // transparent small, animated
    case 31: // stair helper
        return Role::solid;
    case 4:  // clutter
    case 5:  // weapon
    case 10: // props
    case 19: // debris small
    case 20: // debris large
        return Role::movable;
    default: // biped, triggers, water, non-collidable, picking volumes...
        return Role::ignored;
    }
}

// hkpCollidableQualityType: 0 fixed, 1 keyframed, 2 to 7 moving (debris,
// debris with TOI, moving, critical, bullet, user), 8 character, 9 keyframed
// reporting. (nif.xml counts from 1; vanilla data does not fit that.)
constexpr std::int64_t k_quality_keyframed = 1;
constexpr std::int64_t k_quality_moving_first = 2;
constexpr std::int64_t k_quality_moving_last = 7;
constexpr std::int64_t k_quality_keyframed_report = 9;

using godot::real_t;

double number(const Dictionary& d, const char* key, double fallback = 0.0) {
    const Variant v = d.get(key, Variant());
    return v.get_type() == Variant::FLOAT || v.get_type() == Variant::INT ? static_cast<double>(v)
                                                                           : fallback;
}

real_t real(const Variant& v) { return static_cast<real_t>(static_cast<double>(v)); }

/// A three-element array as a vector, scaled; `ok` false if it is not one.
Vector3 vec3(const Dictionary& d, const char* key, bool& ok) {
    const Variant v = d.get(key, Variant());
    if (v.get_type() != Variant::ARRAY || Array(v).size() != 3) {
        ok = false;
        return {};
    }
    const Array a = v;
    return Vector3(real(a[0]), real(a[1]), real(a[2])) * k_havok_scale;
}

/// The flat `vertices` list as points in game units.
godot::PackedVector3Array points_of(const Dictionary& d) {
    godot::PackedVector3Array out;
    const Variant v = d.get("vertices", Variant());
    if (v.get_type() != Variant::ARRAY) {
        return out;
    }
    const Array flat = v;
    const std::int64_t count = flat.size() / 3;
    out.resize(count);
    for (std::int64_t i = 0; i < count; ++i) {
        out.set(i, Vector3(real(flat[i * 3]), real(flat[i * 3 + 1]), real(flat[i * 3 + 2])) *
                       k_havok_scale);
    }
    return out;
}

bool finite(const Vector3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// The direction in which `points` are thinnest (smallest principal axis,
/// by Jacobi rotations of their covariance) and their extent along it.
std::pair<Vector3, real_t> thinnest_axis(const godot::PackedVector3Array& points) {
    Vector3 mean;
    for (const Vector3& p : points) {
        mean += p;
    }
    mean /= static_cast<real_t>(points.size());
    double a[3][3] = {};
    for (const Vector3& p : points) {
        const Vector3 d = p - mean;
        const double v[3] = {static_cast<double>(d.x), static_cast<double>(d.y), static_cast<double>(d.z)};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                a[i][j] += v[i] * v[j];
            }
        }
    }
    double e[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int sweep = 0; sweep < 16; ++sweep) {
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::abs(a[p][q]) < 1e-12) {
                    continue;
                }
                const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1));
                const double c = 1 / std::sqrt(t * t + 1);
                const double s = t * c;
                for (int k = 0; k < 3; ++k) { // a = J^T a J
                    const double akp = a[k][p];
                    const double akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double apk = a[p][k];
                    const double aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double ekp = e[k][p];
                    const double ekq = e[k][q];
                    e[k][p] = c * ekp - s * ekq;
                    e[k][q] = s * ekp + c * ekq;
                }
            }
        }
    }
    int least = 0;
    for (int i = 1; i < 3; ++i) {
        if (a[i][i] < a[least][least]) {
            least = i;
        }
    }
    const Vector3 axis = Vector3(static_cast<real_t>(e[0][least]), static_cast<real_t>(e[1][least]),
                                 static_cast<real_t>(e[2][least]))
                             .normalized();
    real_t lo = std::numeric_limits<real_t>::max();
    real_t hi = std::numeric_limits<real_t>::lowest();
    for (const Vector3& p : points) {
        const real_t d = axis.dot(p);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    return {axis, hi - lo};
}

/// Havok gives flat hulls (load door planes, rugs) their thickness through
/// the convex radius; Jolt cannot build a hull without volume. Make them a
/// slab: the points pushed out both ways along the thin axis.
void thicken(godot::PackedVector3Array& points, real_t radius) {
    constexpr real_t k_min_thickness = 2.0F; // game units
    const auto [axis, thickness] = thinnest_axis(points);
    if (thickness >= k_min_thickness) {
        return;
    }
    const Vector3 offset = axis * std::max(radius, k_min_thickness / 2);
    godot::PackedVector3Array slab;
    for (const Vector3& p : points) {
        slab.push_back(p + offset);
        slab.push_back(p - offset);
    }
    points = slab;
}

/// One extras entry as a shape; false if it has no usable geometry.
bool parse_shape(const Dictionary& d, ModelCollision::Shape& out) {
    const godot::String kind = d.get("kind", godot::String());
    bool ok = true;

    const Variant xform = d.get("transform", Variant());
    if (xform.get_type() == Variant::DICTIONARY) {
        const Dictionary t = xform;
        const Vector3 origin = vec3(t, "translation", ok);
        const Variant r = t.get("rotation", Variant());
        godot::Basis basis;
        if (r.get_type() == Variant::ARRAY && Array(r).size() == 4) {
            const Array q = r;
            const godot::Quaternion quat(real(q[0]), real(q[1]), real(q[2]), real(q[3]));
            if (quat.length_squared() > 0.5F) {
                basis = godot::Basis(quat.normalized());
            }
        }
        out.transform = Transform3D(basis, ok ? origin : Vector3());
        ok = true;
    }

    const real_t radius = static_cast<real_t>(number(d, "radius")) * k_havok_scale;
    if (kind == "box") {
        out.kind = ModelCollision::Kind::box;
        out.half_extents = vec3(d, "half_extents", ok) + Vector3(radius, radius, radius);
        return ok && finite(out.half_extents) && out.half_extents.x > 0 && out.half_extents.y > 0 &&
               out.half_extents.z > 0;
    }
    if (kind == "sphere") {
        out.kind = ModelCollision::Kind::sphere;
        out.radius = radius;
        return std::isfinite(radius) && radius > 0;
    }
    if (kind == "capsule" || kind == "cylinder") {
        const bool cylinder = kind == "cylinder";
        const Vector3 a = vec3(d, "point_a", ok);
        const Vector3 b = vec3(d, "point_b", ok);
        if (!ok || !finite(a) || !finite(b) || !std::isfinite(radius) || radius <= 0) {
            return false;
        }
        const Vector3 axis = b - a;
        const real_t length = axis.length();
        out.radius = radius;
        if (length < 1e-4F && !cylinder) {
            out.kind = ModelCollision::Kind::sphere;
            out.transform = out.transform * Transform3D(godot::Basis(), (a + b) / 2);
            return true;
        }
        if (cylinder && length < 1e-4F) {
            return false;
        }
        // Godot's capsules and cylinders run along Y; a capsule counts its
        // caps in the height.
        out.kind = cylinder ? ModelCollision::Kind::cylinder : ModelCollision::Kind::capsule;
        out.height = cylinder ? length : length + 2 * radius;
        out.transform = out.transform *
                        Transform3D(godot::Basis(godot::Quaternion(Vector3(0, 1, 0), axis / length)),
                                    (a + b) / 2);
        return true;
    }
    if (kind == "convex_vertices") {
        out.kind = ModelCollision::Kind::convex;
        out.points = points_of(d);
        if (out.points.size() < 3 || !std::isfinite(radius)) {
            return false;
        }
        thicken(out.points, radius);
        return true;
    }
    if (kind == "compressed_mesh" || kind == "mesh") {
        out.kind = ModelCollision::Kind::mesh;
        const godot::PackedVector3Array vertices = points_of(d);
        const Variant v = d.get("indices", Variant());
        if (v.get_type() != Variant::ARRAY) {
            return false;
        }
        const Array indices = v;
        const std::int64_t count = vertices.size();
        for (std::int64_t i = 0; i + 2 < indices.size(); i += 3) {
            const std::int64_t a = indices[i];
            const std::int64_t b = indices[i + 1];
            const std::int64_t c = indices[i + 2];
            if (a < 0 || b < 0 || c < 0 || a >= count || b >= count || c >= count || a == b ||
                b == c || a == c) {
                continue;
            }
            out.points.push_back(vertices[a]);
            out.points.push_back(vertices[b]);
            out.points.push_back(vertices[c]);
        }
        return !out.points.is_empty();
    }
    return false; // unsupported
}

/// The bethconv block of an extras dictionary, or {}.
Dictionary bethconv_block(const Variant& extras) {
    if (extras.get_type() != Variant::DICTIONARY) {
        return {};
    }
    const Variant block = Dictionary(extras).get("bethconv", Variant());
    return block.get_type() == Variant::DICTIONARY ? Dictionary(block) : Dictionary();
}

/// The body in one node's collision extras, if it collides; removes them.
void take_body(Dictionary block, const godot::NodePath& node, const godot::StringName& bone,
               std::vector<ModelCollision::Body>& out) {
    const Variant entries = block.get("collision", Variant());
    if (entries.get_type() == Variant::ARRAY) {
        // Every entry of a node comes from its one rigid body.
        const Array list = entries;
        ModelCollision::Body body;
        body.node = node;
        body.bone = bone;
        Role role = Role::ignored;
        std::int64_t quality = 0;
        for (std::int64_t i = 0; i < list.size(); ++i) {
            if (list[i].get_type() != Variant::DICTIONARY) {
                continue;
            }
            const Dictionary d = list[i];
            if (i == 0) {
                const auto layer = static_cast<std::int64_t>(number(d, "layer"));
                role = role_of(layer);
                quality = static_cast<std::int64_t>(number(d, "quality_type"));
                body.mass = number(d, "mass", 1.0);
                body.friction = number(d, "friction", 0.5);
                body.restitution = number(d, "restitution", 0.4);
                if (layer == 2 || layer == 28) {
                    body.motion = ModelCollision::Motion::animated;
                }
            }
            ModelCollision::Shape shape;
            if (parse_shape(d, shape)) {
                body.shapes.push_back(std::move(shape));
            }
        }
        // Packs before mesh/10 have no quality: everything stays fixed.
        if (quality == k_quality_keyframed || quality == k_quality_keyframed_report) {
            body.motion = ModelCollision::Motion::animated;
        } else if (role == Role::movable && quality >= k_quality_moving_first &&
                   quality <= k_quality_moving_last) {
            body.motion = ModelCollision::Motion::dynamic;
        }
        if (role != Role::ignored && !body.shapes.empty()) {
            out.push_back(std::move(body));
        }
        // The arrays are large and nothing else reads them.
        block.erase("collision");
    }
}

void collect(godot::Node* root, godot::Node* node, std::vector<ModelCollision::Body>& out,
             int depth) {
    if (depth > 64) {
        return;
    }
    const godot::NodePath path = root->get_path_to(node);
    if (auto* skeleton = godot::Object::cast_to<godot::Skeleton3D>(node)) {
        // NIF nodes that skin a mesh became bones; their extras are bone
        // metadata, the root bone's also the skeleton's own. The shapes are
        // in the bone's space.
        bool bones = false;
        for (std::int32_t i = 0; i < skeleton->get_bone_count(); ++i) {
            if (skeleton->has_bone_meta(i, "extras")) {
                bones = true;
                take_body(bethconv_block(skeleton->get_bone_meta(i, "extras")), path,
                          skeleton->get_bone_name(i), out);
            }
        }
        if (bones) {
            bethconv_extras(node).erase("collision");
        }
    }
    take_body(bethconv_extras(node), path, godot::StringName(), out);
    for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
        collect(root, node->get_child(i), out, depth + 1);
    }
}

} // namespace

std::shared_ptr<ModelCollision> ModelCollision::take_from(godot::Node* root) {
    if (root == nullptr) {
        return {};
    }
    auto out = std::make_shared<ModelCollision>();
    collect(root, root, out->bodies_, 0);
    if (out->bodies_.empty()) {
        return {};
    }
    // One movable body made of convex shapes can simulate; anything else
    // (several bodies, a mesh shape, a constraint we do not convert) stays put.
    const bool simulate =
        out->bodies_.size() == 1 && out->bodies_[0].motion == Motion::dynamic &&
        std::ranges::none_of(out->bodies_[0].shapes, [](const Shape& s) { return s.kind == Kind::mesh; });
    for (Body& body : out->bodies_) {
        if (body.motion == Motion::dynamic && !simulate) {
            body.motion = Motion::fixed;
        }
    }
    return out;
}

const std::vector<Ref<godot::Shape3D>>& ModelCollision::shapes_of(std::size_t index) const {
    if (made_.size() != bodies_.size()) {
        made_.resize(bodies_.size());
    }
    auto& made = made_[index];
    if (!made.empty()) {
        return made;
    }
    for (const Shape& shape : bodies_[index].shapes) {
        Ref<godot::Shape3D> out;
        switch (shape.kind) {
        case Kind::box: {
            Ref<godot::BoxShape3D> box;
            box.instantiate();
            box->set_size(shape.half_extents * 2);
            out = box;
            break;
        }
        case Kind::sphere: {
            Ref<godot::SphereShape3D> sphere;
            sphere.instantiate();
            sphere->set_radius(static_cast<float>(shape.radius));
            out = sphere;
            break;
        }
        case Kind::capsule: {
            Ref<godot::CapsuleShape3D> capsule;
            capsule.instantiate();
            capsule->set_radius(static_cast<float>(shape.radius));
            capsule->set_height(static_cast<float>(shape.height));
            out = capsule;
            break;
        }
        case Kind::cylinder: {
            Ref<godot::CylinderShape3D> cylinder;
            cylinder.instantiate();
            cylinder->set_radius(static_cast<float>(shape.radius));
            cylinder->set_height(static_cast<float>(shape.height));
            out = cylinder;
            break;
        }
        case Kind::convex: {
            Ref<godot::ConvexPolygonShape3D> convex;
            convex.instantiate();
            convex->set_points(shape.points);
            out = convex;
            break;
        }
        case Kind::mesh: {
            Ref<godot::ConcavePolygonShape3D> mesh;
            mesh.instantiate();
            // Havok meshes collide from both sides.
            mesh->set_backface_collision_enabled(true);
            mesh->set_faces(shape.points);
            out = mesh;
            break;
        }
        }
        made.push_back(out);
    }
    return made;
}

int ModelCollision::attach(godot::Node3D* instance) const {
    if (instance == nullptr) {
        return 0;
    }
    int count = 0;
    for (std::size_t i = 0; i < bodies_.size(); ++i) {
        const Body& body = bodies_[i];
        auto* owner = godot::Object::cast_to<godot::Node3D>(instance->get_node_or_null(body.node));
        if (owner == nullptr) {
            continue;
        }
        if (!body.bone.is_empty()) {
            auto* skeleton = godot::Object::cast_to<godot::Skeleton3D>(owner);
            if (skeleton == nullptr || skeleton->find_bone(body.bone) < 0) {
                continue;
            }
            auto* attachment = memnew(godot::BoneAttachment3D);
            attachment->set_name(godot::String(body.bone) + " Collision");
            skeleton->add_child(attachment);
            attachment->set_bone_name(body.bone);
            // Placed now, not only on the skeleton's next pose update: a
            // dynamic body reads its place when it is added.
            attachment->set_transform(skeleton->get_bone_global_pose(skeleton->find_bone(body.bone)));
            owner = attachment;
        }
        godot::CollisionObject3D* object = nullptr;
        switch (body.motion) {
        case Motion::fixed:
            object = memnew(godot::StaticBody3D);
            object->set_collision_layer(physics_layer::world);
            object->set_collision_mask(0);
            break;
        case Motion::animated: {
            auto* animated = memnew(godot::AnimatableBody3D);
            // Synced to physics, the body follows only its own local
            // transform; clips move the node it hangs off (a door's leaf),
            // so it would stay shut. Unsynced it follows its global one.
            animated->set_sync_to_physics(false);
            object = animated;
            object->set_collision_layer(physics_layer::world);
            object->set_collision_mask(0);
            break;
        }
        case Motion::dynamic: {
            auto* dynamic = memnew(SkydotDynamicBody);
            dynamic->set_model(instance);
            dynamic->set_mass(static_cast<float>(std::max(body.mass, 0.1)));
            Ref<godot::PhysicsMaterial> material;
            material.instantiate();
            material->set_friction(static_cast<float>(std::clamp(body.friction, 0.0, 1.0)));
            material->set_bounce(static_cast<float>(std::clamp(body.restitution, 0.0, 1.0)));
            dynamic->set_physics_material_override(material);
            dynamic->set_collision_layer(physics_layer::clutter);
            dynamic->set_collision_mask(physics_layer::solid | physics_layer::actor);
            object = dynamic;
            break;
        }
        }
        object->set_name("Collision");
        const auto& shapes = shapes_of(i);
        for (std::size_t s = 0; s < shapes.size(); ++s) {
            auto* node = memnew(godot::CollisionShape3D);
            node->set_shape(shapes[s]);
            node->set_transform(body.shapes[s].transform);
            object->add_child(node);
        }
        owner->add_child(object);
        ++count;
    }
    return count;
}

// ---- SkydotDynamicBody -----------------------------------------------------

void SkydotDynamicBody::_bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("wake"), &SkydotDynamicBody::wake);
}

void SkydotDynamicBody::_ready() {
    auto* owner = godot::Object::cast_to<godot::Node3D>(get_parent());
    if (owner == nullptr) {
        return;
    }
    // Rigid bodies cannot be scaled: simulate in world space without scale
    // and scale the shapes instead.
    const Transform3D placed = owner->get_global_transform();
    const Vector3 scales = placed.basis.get_scale();
    const real_t scale = (scales.x + scales.y + scales.z) / 3;
    const Transform3D body = placed.orthonormalized();
    set_as_top_level(true);
    set_global_transform(body);
    for (std::int32_t i = 0; i < get_child_count(); ++i) {
        if (auto* shape = godot::Object::cast_to<godot::CollisionShape3D>(get_child(i))) {
            const Transform3D t = shape->get_transform();
            shape->set_transform(Transform3D(t.basis.scaled(Vector3(scale, scale, scale)), t.origin * scale));
        }
    }
    if (model_ != nullptr) {
        model_offset_ = body.affine_inverse() * model_->get_global_transform();
    }
    // Frozen as placed until something touches it: Jolt activates bodies as
    // they enter the space, so sleeping would not hold.
    set_freeze_mode(FREEZE_MODE_STATIC);
    set_freeze_enabled(true);
}

int wake_clutter(godot::Node* root, const Vector3& centre, godot::real_t radius) {
    if (root == nullptr) {
        return 0;
    }
    int count = 0;
    std::vector<godot::Node*> pending{root};
    while (!pending.empty()) {
        godot::Node* node = pending.back();
        pending.pop_back();
        if (auto* body = godot::Object::cast_to<SkydotDynamicBody>(node)) {
            if (body->is_freeze_enabled() && body->is_inside_tree() &&
                body->get_global_position().distance_to(centre) <= radius) {
                body->wake();
                ++count;
            }
            continue;
        }
        for (std::int32_t i = 0; i < node->get_child_count(); ++i) {
            pending.push_back(node->get_child(i));
        }
    }
    return count;
}

void SkydotDynamicBody::wake() {
    if (is_freeze_enabled()) {
        set_freeze_enabled(false);
        // Whatever frozen clutter it runs into starts moving too.
        set_contact_monitor(true);
        set_max_contacts_reported(4);
        const auto entered = callable_mp(this, &SkydotDynamicBody::on_body_entered);
        if (!is_connected("body_entered", entered)) {
            connect("body_entered", entered);
        }
    }
    set_sleeping(false);
}

void SkydotDynamicBody::on_body_entered(godot::Node* other) {
    if (auto* clutter = godot::Object::cast_to<SkydotDynamicBody>(other)) {
        clutter->wake();
    }
}

void SkydotDynamicBody::_integrate_forces(godot::PhysicsDirectBodyState3D* state) {
    // Called for awake bodies only, so resting clutter costs nothing.
    if (model_ != nullptr) {
        model_->set_global_transform(state->get_transform() * model_offset_);
    }
}

} // namespace skydot
