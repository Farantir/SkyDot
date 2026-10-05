// SPDX-License-Identifier: GPL-3.0-or-later
//
// Physics bodies from the Havok collision the converter keeps in mesh extras
// (formats/pack-format.md, "Collision in meshes").
//
// `ModelCollision` is parsed once per model on the loader's thread and takes
// the (large) extras off the template; its Godot shapes are created on first
// use, on the main thread, and shared by every instance. `attach` gives an
// instance its bodies:
//
// - fixed bodies become a StaticBody3D under the node that owns them, so they
//   carry the reference's scale (uniform, which both physics engines take);
// - keyframed and animated-static ones an AnimatableBody3D there, so doors
//   and gates move with their animation;
// - a model whose only body is movable clutter becomes a `SkydotDynamicBody`
//   that the model follows; it starts frozen, so clutter stays where the
//   game put it until something touches it.
//
// Shapes are in game units (Havok units times `k_havok_scale`), the unit of
// the nodes they hang off.
#pragma once

#include "skydot_formats/units.hpp"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/physics_direct_body_state3d.hpp>
#include <godot_cpp/classes/rigid_body3d.hpp>
#include <godot_cpp/classes/shape3d.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace skydot {

/// Godot collision layer bits.
namespace physics_layer {
inline constexpr std::uint32_t world = 1;   ///< Static and animated geometry.
inline constexpr std::uint32_t clutter = 2; ///< Movable objects.
inline constexpr std::uint32_t actor = 4;   ///< The player.
inline constexpr std::uint32_t terrain = 8; ///< Exterior land.
inline constexpr std::uint32_t npc = 16;    ///< Placed actors (SkydotActor).
/// What walking things and clutter collide with.
inline constexpr std::uint32_t solid = world | clutter | terrain;
} // namespace physics_layer

/// Game units per Havok unit in Skyrim (LE and SE).
inline constexpr godot::real_t k_havok_scale = static_cast<godot::real_t>(formats::k_havok_scale);

class ModelCollision {
public:
    enum class Kind : std::uint8_t { box, sphere, capsule, cylinder, convex, mesh };
    enum class Motion : std::uint8_t { fixed, animated, dynamic };

    struct Shape {
        Kind kind{Kind::box};
        godot::Transform3D transform; ///< In the owning node's frame, game units.
        godot::Vector3 half_extents;  ///< box, convex radius included
        godot::real_t radius{};       ///< sphere, capsule, cylinder
        godot::real_t height{};       ///< capsule, end to end (Godot's); cylinder
        godot::PackedVector3Array points; ///< convex points, or mesh faces
    };

    struct Body {
        godot::NodePath node; ///< From the model's root to the owning node.
        /// When the owner is a NIF node Godot made a bone of: `node` is the
        /// Skeleton3D and this the bone, which places the shapes.
        godot::StringName bone;
        Motion motion{Motion::fixed};
        double mass{1.0};
        double friction{0.5};
        double restitution{0.4};
        std::vector<Shape> shapes;
    };

    /// Read `root`'s collision extras into bodies and remove them from the
    /// nodes. Null if nothing collides. Touches no server; safe on a worker.
    static std::shared_ptr<ModelCollision> take_from(godot::Node* root);

    /// Give `instance` (a copy of the model) its bodies. Returns how many.
    /// Main thread only.
    int attach(godot::Node3D* instance) const;

    [[nodiscard]] const std::vector<Body>& bodies() const { return bodies_; }

private:
    const std::vector<godot::Ref<godot::Shape3D>>& shapes_of(std::size_t body) const;

    std::vector<Body> bodies_;
    /// Per body, created on first `attach`.
    mutable std::vector<std::vector<godot::Ref<godot::Shape3D>>> made_;
};

/// Movable clutter. Placed under the model's owning node but simulated in
/// world space; it moves `model` (the reference's root) along. Frozen as
/// placed until `wake`: the player wakes what it walks into, and woken
/// clutter wakes frozen clutter it hits.
class SkydotDynamicBody : public godot::RigidBody3D {
    GDCLASS(SkydotDynamicBody, godot::RigidBody3D)

public:
    /// `model` is the reference's root; shapes are its CollisionShape3D
    /// children with transforms in the owning node's frame (game units).
    void set_model(godot::Node3D* model) { model_ = model; }

    /// Unfreeze and start simulating.
    void wake();

    void _ready() override;
    void _integrate_forces(godot::PhysicsDirectBodyState3D* state) override;

protected:
    static void _bind_methods();

private:
    void on_body_entered(godot::Node* other);

    godot::Node3D* model_{nullptr};
    godot::Transform3D model_offset_; ///< Body to model root.
};

/// Wake every frozen clutter body under `root` whose centre is within
/// `radius` of `centre` (Godot space), as when what it rests on goes away.
/// Returns how many.
int wake_clutter(godot::Node* root, const godot::Vector3& centre, godot::real_t radius);

} // namespace skydot
