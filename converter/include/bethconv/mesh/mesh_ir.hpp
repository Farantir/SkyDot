// SPDX-License-Identifier: GPL-3.0-or-later
//
// Intermediate representation between NIF and glTF.
//
// NIF is Z-up in game units with per-shape shader properties, nine texture
// slots and per-shape bone lists; glTF is Y-up in metres with a material array,
// PBR channels and one skin per mesh. The IR separates reading from writing so
// later questions are about the model, not nifly's API.
//
// Plain structs, no logic. Bethesda-specific data without a glTF equivalent is
// kept in the material and collision structs; the writer decides what becomes
// PBR and what goes to `extras`.
#pragma once

#include "skydot_formats/units.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bethconv::mesh {

struct Vec2 {
    float x{}, y{};
};
struct Vec3 {
    float x{}, y{}, z{};
};
struct Vec4 {
    float x{}, y{}, z{}, w{};
};

/// Column-major 4x4 (glTF's convention).
struct Mat4 {
    std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

/// Translation / rotation (quaternion xyzw) / scale, as in a glTF node.
struct Transform {
    Vec3 translation{0, 0, 0};
    Vec4 rotation{0, 0, 0, 1};
    Vec3 scale{1, 1, 1};
};

/// The BS shader property a material came from. Slot meanings differ: slot 1
/// is a normal map under Lighting and a greyscale palette under Effect.
enum class ShaderKind : std::uint8_t {
    none,      ///< No shader property at all; a placeholder material is emitted.
    lighting,  ///< BSLightingShaderProperty (most shapes).
    effect,    ///< BSEffectShaderProperty -- decals, glow, water, fx planes.
    water,     ///< BSWaterShaderProperty: placed water (streams, ponds).
    other,     ///< Another BSShaderProperty subclass, not modeled yet.
};

/// glTF alpha mode, derived from NiAlphaProperty's blend bitfield.
enum class AlphaMode : std::uint8_t { opaque, mask, blend };

/// Bethesda's nine texture slots, by index. Slot roles depend on the shader
/// type (slot 2 is glow, skin tint or detail mask), so they are resolved where
/// used rather than named here.
using TextureSlots = std::array<std::string, 9>;

/// One shape's surface. PBR fields for glTF, plus all Bethesda fields kept for
/// an engine-side shader.
struct Material {
    std::string name;
    ShaderKind kind{ShaderKind::none};
    std::uint32_t bs_shader_type{}; ///< BSLightingShaderProperty::skyrimShaderType.

    TextureSlots textures{};

    // --- maps onto glTF PBR ---
    Vec4 base_color{1, 1, 1, 1}; ///< Alpha carries NiAlphaProperty/shader alpha.
    AlphaMode alpha_mode{AlphaMode::opaque};
    float alpha_cutoff{0.5f};
    bool double_sided{false};
    Vec3 emissive{0, 0, 0};
    float emissive_strength{1.0f}; ///< KHR_materials_emissive_strength when > 1.

    // --- no glTF equivalent; written to `extras` ---
    float glossiness{};
    float specular_strength{};
    Vec3 specular_color{1, 1, 1};
    Vec2 uv_offset{0, 0};
    Vec2 uv_scale{1, 1};
    float environment_map_scale{};
    bool model_space_normals{false}; ///< `_msn`: normals in model space.
    bool skin_tinted{false};
    bool face_tinted{false};
    /// HairTint shaders (type 6): the colour the hair is tinted to. FaceGen
    /// heads carry the NPC's hair colour here (the Creation Kit bakes it in).
    std::optional<Vec3> hair_tint;
    /// SkinTint shaders (type 5): the tint stored in the NIF.
    std::optional<Vec3> skin_tint;
    bool has_glowmap{false};
    bool has_environment_map{false};
    bool has_backlight{false};
    bool has_rimlight{false};
    bool has_softlight{false};
    bool decal{false};
    bool alpha_property_present{false};
    std::uint16_t alpha_flags{};
    /// BSShaderProperty flags, as stored (nifly's SLSF1_* / SLSF2_*).
    std::uint32_t shader_flags1{};
    std::uint32_t shader_flags2{};
    /// SLSF1_REFRACTION: the normal map distorts what is behind the surface
    /// (heat haze, water). Not a visible surface in glTF terms.
    bool refraction{false};
    /// Emissive color alpha (effect shaders square it into their alpha).
    float emissive_alpha{1.0f};
    /// BSEffectShaderProperty falloff: start/stop angle (as cosines) and
    /// start/stop opacity. Only meaningful with SLSF1_USE_FALLOFF.
    Vec4 falloff{1.0f, 1.0f, 0.0f, 0.0f};
    float soft_falloff_depth{};
    /// BSEffectShaderProperty lighting influence (0..1): how far Effect_Lighting
    /// moves the colour from unlit to lit.
    float lighting_influence{};
};

/// One drawable shape: a triangle list with per-vertex attributes. Every
/// attribute is empty or exactly `positions.size()` long (enforced by the
/// reader).
struct Primitive {
    std::string name;                ///< NIF shape name; generated if blank.
    std::size_t material{};          ///< Index into Model::materials.
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec4> tangents;      ///< xyz = tangent, w = bitangent sign.
    std::vector<Vec2> uvs;
    std::vector<Vec4> colors;        ///< Linear RGBA (glTF COLOR_0).
    std::vector<std::array<std::uint16_t, 4>> joints;
    std::vector<Vec4> weights;
    std::vector<std::uint32_t> indices;

    std::optional<std::size_t> skin; ///< Index into Model::skins.

    /// LOD level from BSLODTriShape, if any.
    std::optional<std::uint32_t> lod_level;
};

/// A scene graph node. The NIF hierarchy is kept, not flattened: attachment
/// points, animation targets and collision hang off named nodes.
struct Node {
    std::string name;
    Transform transform;
    std::vector<std::size_t> children;      ///< Indices into Model::nodes.
    std::vector<std::size_t> primitives;    ///< Indices into Model::primitives.
    bool is_joint{false};
    /// NiBillboardNode mode (nifly BillboardMode); the engine turns the node
    /// towards the camera each frame.
    std::optional<std::uint16_t> billboard_mode;
    /// Position among a BSOrderedNode's children: the game draws them in
    /// that order (glass, then the liquid in it), not by depth.
    std::optional<std::uint32_t> draw_order;
    /// BSValueNode named "AddOnNode...": the game attaches the model of the
    /// ADDN record with this index here (candle flames, smoke).
    std::optional<std::int32_t> addon_index;
    /// NiAVObject flag bit 0: not drawn until a controller shows it (flames
    /// switched by a NiVisController, emitter meshes).
    bool hidden{false};
    /// Animations or particles refer to this node, so its extras carry the
    /// node index as a stable id (engines rename nodes).
    bool referenced{false};
};

/// How keys are interpolated. TBC keys are converted to `cubic` (Hermite with
/// explicit tangents) by the reader.
enum class KeyInterp : std::uint8_t { step, linear, cubic };

/// One animated property of one node.
///
/// `property` names what is driven: `translation`, `rotation` (quaternion
/// xyzw), `rotation_x/_y/_z` (Euler angles, applied X then Y then Z), `scale`,
/// `visible`, `alpha_test_ref`, `effect.<var>`, `lighting.<var>` (shader
/// variables on the node's material), `particles.birth_rate`,
/// `particles.active`. Values are `components` floats per key.
struct AnimationChannel {
    std::size_t node{};
    std::string property;
    KeyInterp interp{KeyInterp::linear};
    std::uint8_t components{1};
    std::vector<float> times;
    std::vector<float> values;
    /// Only for `cubic`: incoming and outgoing tangents, per key and component.
    std::vector<float> in_tangents;
    std::vector<float> out_tangents;
};

/// NiTimeController / NiControllerSequence cycle type.
enum class CycleMode : std::uint8_t { loop, reverse, clamp };

/// A set of channels that play on one clock. Controllers outside a
/// NiControllerManager run by themselves from load; they become unnamed clips
/// grouped by timing. Each NiControllerSequence becomes a named clip (`Open`,
/// `Close`, `mIdle`) that the engine plays on request.
struct AnimationClip {
    std::string name;
    bool autoplay{false};
    CycleMode cycle{CycleMode::loop};
    float frequency{1.0f};
    float phase{0.0f};
    float start{0.0f};
    float stop{0.0f};
    std::vector<AnimationChannel> channels;
    /// NiTextKeyExtraData: (time, text), e.g. sound cues and `start`/`end`.
    std::vector<std::pair<float, std::string>> text_keys;
};

enum class EmitterKind : std::uint8_t { box, sphere, cylinder, mesh };

/// NiPSysEmitter and its volume or mesh subclass.
struct ParticleEmitter {
    EmitterKind kind{EmitterKind::box};
    std::optional<std::size_t> node; ///< Emitter object; the particle node if none.
    Vec3 size{};                     ///< box w/h/d; sphere r; cylinder r, h.
    std::vector<std::size_t> meshes; ///< mesh: nodes whose vertices emit.
    std::uint32_t mesh_velocity{};   ///< nifly VelocityType.
    Vec3 mesh_axis{};
    float speed{}, speed_variation{};
    float declination{}, declination_variation{};
    float planar_angle{}, planar_angle_variation{};
    Vec4 color{1, 1, 1, 1};
    float radius{}, radius_variation{};
    float life_span{}, life_span_variation{};
    /// Particles per second when the emitter controller has no keys.
    float birth_rate{};
};

struct ParticleGravity {
    std::optional<std::size_t> node;
    Vec3 axis{0, 0, 1};
    float strength{}, decay{};
    bool spherical{false};
    float turbulence{}, turbulence_scale{1.0f};
    bool world_aligned{false};
};

/// NiPSysDragModifier: slows velocity along `axis` of `node` (all directions
/// when the axis is zero), full strength within `range`, fading out over
/// `range_falloff` beyond it.
struct ParticleDrag {
    std::optional<std::size_t> node;
    Vec3 axis{};
    float percentage{}, range{}, range_falloff{};
};

/// One NiParticleSystem. Only modifiers that change how particles look or
/// move are kept; the rest are listed by name in `unsupported`.
struct ParticleSystem {
    std::size_t node{};
    std::size_t material{};
    bool world_space{true};
    std::uint32_t max_particles{};
    bool strip{false}; ///< BSStripParticleSystem: rendered as plain particles.
    std::vector<ParticleEmitter> emitters;
    std::vector<ParticleGravity> gravity;

    bool has_rotation{false};
    float rotation_speed{}, rotation_speed_variation{};
    float rotation_angle{}, rotation_angle_variation{};
    bool rotation_random_sign{false};

    /// BSPSysScaleModifier: size multipliers spread evenly over the lifetime.
    std::vector<float> scales;
    /// NiPSysGrowFadeModifier, in seconds.
    float grow_time{}, fade_time{};
    float base_scale{1.0f};

    /// BSPSysSimpleColorModifier.
    bool has_simple_color{false};
    float fade_in{}, fade_out{};
    float color1_end{}, color2_start{}, color2_end{}, color3_start{};
    std::array<Vec4, 3> colors{};
    /// NiPSysColorModifier keys: time (0..1 of the lifetime) and RGBA.
    std::vector<std::pair<float, Vec4>> color_keys;

    float drag{}, drag_range{};
    std::vector<ParticleDrag> drags;

    /// BSPSysSubTexModifier and NiPSysData's sub-texture rectangles.
    bool has_subtex{false};
    float subtex_start{}, subtex_start_variation{}, subtex_end{};
    float subtex_loop_start{}, subtex_loop_start_variation{};
    float subtex_frame_count{}, subtex_frame_count_variation{};
    std::vector<Vec4> subtex_offsets;

    std::vector<std::string> unsupported;
};

/// A skin: the joint set a skinned primitive binds to.
struct Skin {
    std::string name;
    std::vector<std::size_t> joints;        ///< Indices into Model::nodes.
    std::vector<Mat4> inverse_bind_matrices;
};

/// Supported Havok shape kinds. Others are `unsupported` with their block name,
/// so report.json can count them.
enum class CollisionKind : std::uint8_t {
    unsupported,
    box,
    sphere,
    capsule,
    convex_vertices,
    compressed_mesh,
    list,       ///< Container; children are listed as separate shapes.
    cylinder,   ///< Its convex radius included in `radius` and the end points.
    mesh,       ///< bhkNiTriStripsShape's triangles.
};

/// Game units per Havok unit in Skyrim.
inline constexpr float k_havok_scale = static_cast<float>(skydot::formats::k_havok_scale);

/// One collision shape in the target node's frame, in Havok units (game units
/// divided by `k_havok_scale`). Goes to `extras`, never glTF geometry (which would
/// render); the engine builds physics shapes from it.
struct CollisionShape {
    CollisionKind kind{CollisionKind::unsupported};
    std::string block_name;      ///< Havok block name, always recorded.
    std::string target_node;     ///< Node the bhkCollisionObject hangs off.
    std::uint32_t havok_material{};
    std::uint8_t layer{};
    std::uint8_t motion_type{};
    std::uint8_t quality_type{}; ///< hkpCollidableQualityType: 0 fixed, 1 keyframed, 2-7 moving.
    float mass{};                ///< kg; 0 for fixed bodies.
    float friction{};
    float restitution{};

    Vec3 half_extents{};         ///< box
    float radius{};              ///< sphere, capsule, cylinder
    Vec3 point_a{}, point_b{};   ///< capsule, cylinder
    std::vector<Vec3> vertices;  ///< convex_vertices, compressed_mesh, mesh
    std::vector<std::uint32_t> indices; ///< compressed_mesh, mesh
    Transform transform{};       ///< Body and shape transforms, composed.
};

/// Everything one NIF becomes.
struct Model {
    std::string source;          ///< Virtual path, for diagnostics and extras.
    std::string nif_version;     ///< Version string as the file states it.
    std::uint32_t nif_stream{};  ///< User version 2: 83 = LE, 100 = SE/VR.

    std::vector<Node> nodes;
    std::vector<std::size_t> roots;
    std::vector<Primitive> primitives;
    std::vector<Material> materials;
    std::vector<Skin> skins;
    std::vector<CollisionShape> collision;
    std::vector<AnimationClip> animations;
    std::vector<ParticleSystem> particles;

    /// Non-fatal issues (missing UVs, unsupported Havok shape, dropped
    /// attribute). A bad shape costs that shape, not the file.
    std::vector<std::string> warnings;
};

} // namespace bethconv::mesh
