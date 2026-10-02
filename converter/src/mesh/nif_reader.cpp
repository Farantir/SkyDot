// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/mesh/nif_reader.hpp"

#include "bethconv/io/span_stream.hpp"

#include "nif_controllers.hpp"

#include <NifFile.hpp>
#include <Geometry.hpp>
#include <Nodes.hpp>
#include <Particles.hpp>
#include <Shaders.hpp>
#include <Skin.hpp>
#include <bhk.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <unordered_map>
#include <utility>

namespace bethconv::mesh {
namespace {

/// Maximum NiNode nesting (see walk()). Larger than the GRUP limit because
/// skeletons are long bone chains; vanilla stays far below it. Deeper subtrees
/// are cut off and reported.
constexpr std::size_t k_max_node_depth = 256;


using io::ErrorKind;
using io::ParseError;

ParseError fail(std::string_view origin, ErrorKind kind, std::string detail) {
    return ParseError{std::string(origin), 0, kind, std::move(detail)};
}

Vec3 to_vec3(const nifly::Vector3& v) { return Vec3{v.x, v.y, v.z}; }
Vec2 to_vec2(const nifly::Vector2& v) { return Vec2{v.u, v.v}; }

/// Vertex colors are passed through unconverted. glTF defines COLOR_0 as linear,
/// but Skyrim uses them as a tint/AO factor; converting would darken meshes.
Vec4 to_vec4(const nifly::Color4& c) { return Vec4{c.r, c.g, c.b, c.a}; }

/// Rotation matrix -> quaternion (xyzw), Shepperd's method. NIF matrices drift,
/// so the naive formula divides by near zero on 180-degree turns and yields
/// NaN.
Vec4 quat_from_matrix(const nifly::Matrix3& m) {
    const float m00 = m[0][0], m01 = m[0][1], m02 = m[0][2];
    const float m10 = m[1][0], m11 = m[1][1], m12 = m[1][2];
    const float m20 = m[2][0], m21 = m[2][1], m22 = m[2][2];

    Vec4 q{};
    const float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }

    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len > 0.0f) {
        q.x /= len;
        q.y /= len;
        q.z /= len;
        q.w /= len;
    } else {
        q = Vec4{0, 0, 0, 1};
    }
    return q;
}

Transform to_transform(const nifly::MatTransform& t) {
    Transform out;
    out.translation = to_vec3(t.translation);
    out.rotation = quat_from_matrix(t.rotation);
    out.scale = Vec3{t.scale, t.scale, t.scale};
    return out;
}

/// True if all transform components are finite. Vanilla SE's
/// `meshes/actors/male/greybeardstatic.nif` has a NaN translation (0xFFFFFFFF)
/// on node `AnimObjectR`, and JSON cannot represent NaN. (A NaN rotation already
/// becomes identity in `quat_from_matrix`.)
[[nodiscard]] bool is_finite(const Transform& t) noexcept {
    return std::isfinite(t.translation.x) && std::isfinite(t.translation.y) &&
           std::isfinite(t.translation.z) && std::isfinite(t.rotation.x) &&
           std::isfinite(t.rotation.y) && std::isfinite(t.rotation.z) &&
           std::isfinite(t.rotation.w) && std::isfinite(t.scale.x) &&
           std::isfinite(t.scale.y) && std::isfinite(t.scale.z);
}

/// MatTransform -> column-major 4x4.
Mat4 to_mat4(const nifly::MatTransform& t) {
    Mat4 out;
    const nifly::Matrix3& r = t.rotation;
    const float s = t.scale;
    // Column c is the image of basis vector c; nifly stores rotation by rows,
    // hence the transposed indexing.
    out.m = {r[0][0] * s, r[1][0] * s, r[2][0] * s, 0.0f,
             r[0][1] * s, r[1][1] * s, r[2][1] * s, 0.0f,
             r[0][2] * s, r[1][2] * s, r[2][2] * s, 0.0f,
             t.translation.x, t.translation.y, t.translation.z, 1.0f};
    return out;
}

/// NiAlphaProperty is a bitfield: bit 0 enables blending, bit 9 alpha testing.
/// glTF cannot express both, so both becomes MASK. Skyrim's blend+test surfaces
/// (foliage, chains, fences) are cutouts; BLEND would sort them wrongly.
AlphaMode alpha_mode_of(const nifly::NiAlphaProperty* alpha, float* out_cutoff) {
    if (alpha == nullptr) {
        return AlphaMode::opaque;
    }
    const bool blend = (alpha->flags & 0x0001u) != 0;
    const bool test = (alpha->flags & 0x0200u) != 0;
    if (test) {
        *out_cutoff = static_cast<float>(alpha->threshold) / 255.0f;
        return AlphaMode::mask;
    }
    return blend ? AlphaMode::blend : AlphaMode::opaque;
}

ShaderKind kind_of(nifly::NiShader* shader) {
    if (shader == nullptr) {
        return ShaderKind::none;
    }
    const std::string_view name = shader->GetBlockName();
    if (name == "BSLightingShaderProperty") {
        return ShaderKind::lighting;
    }
    if (name == "BSEffectShaderProperty") {
        return ShaderKind::effect;
    }
    return ShaderKind::other;
}

/// Nodes named "EditorMarker" hold geometry only the Creation Kit shows.
bool is_editor_marker(std::string_view name) {
    constexpr std::string_view marker = "editormarker";
    return name.size() == marker.size() &&
           std::ranges::equal(name, marker, [](char a, char b) {
               return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
           });
}

/// Builds the IR from one NifFile.
class Reader {
public:
    Reader(nifly::NifFile& nif, Model& model, const ReadOptions& options)
        : nif_(nif), model_(model), opt_(options),
          controllers_(nif, model, node_by_block_, node_by_name_) {}

    void run() {
        nifly::NiNode* root = nif_.GetRootNode();
        if (root == nullptr) {
            // Valid but no root (some effect NIFs are a bare shape): emit the
            // shapes without a graph.
            model_.warnings.emplace_back("no root NiNode; shapes emitted without a graph");
            for (nifly::NiShape* shape : nif_.GetShapes()) {
                const std::size_t node = add_node_for(shape);
                model_.roots.push_back(node);
            }
        } else {
            model_.roots.push_back(walk(root));
        }
        resolve_skins();
        if (opt_.read_animations) {
            controllers_.finish();
        }
    }

private:
    nifly::NifFile& nif_;
    Model& model_;
    const ReadOptions& opt_;

    /// nifly block index -> IR node index, for resolving skin bones later.
    std::unordered_map<std::uint32_t, std::size_t> node_by_block_;
    /// Node name -> IR node index. Names may repeat; first wins, as in the
    /// engine.
    std::unordered_map<std::string, std::size_t> node_by_name_;

    /// A primitive and its source shape, for skinning after the walk.
    struct PendingSkin {
        std::size_t primitive;
        nifly::NiShape* shape;
        std::size_t node;
    };
    std::vector<PendingSkin> pending_skins_;

    detail::ControllerReader controllers_;

    std::size_t add_node(std::string name, const nifly::MatTransform& xform,
                         std::uint32_t block_index) {
        const std::size_t index = model_.nodes.size();
        Node node;
        node.name = std::move(name);
        node.transform = to_transform(xform);
        if (!is_finite(node.transform)) {
            // Replace with identity and warn: NaN would spread into bounds,
            // bind poses and physics.
            model_.warnings.push_back("node '" + node.name +
                                      "' has a non-finite transform; replaced with the identity");
            node.transform = Transform{};
        }
        model_.nodes.push_back(std::move(node));
        node_by_block_.emplace(block_index, index);
        if (!model_.nodes[index].name.empty()) {
            node_by_name_.try_emplace(model_.nodes[index].name, index);
        }
        return index;
    }

    /// Walks a NiAVObject subtree and returns its IR node index.
    ///
    /// Child references are block indices and may point back up the tree, so a
    /// file can describe arbitrarily deep or cyclic graphs. `depth` bounds the
    /// recursion (fuzz_nif found a stack overflow with a 291-byte file).
    std::size_t walk(nifly::NiAVObject* obj, std::size_t depth = 0) {
        const std::uint32_t block = nif_.GetBlockID(obj);
        std::size_t index = add_node(obj->name.get(), obj->transform, block);
        if (const auto* billboard = dynamic_cast<nifly::NiBillboardNode*>(obj)) {
            model_.nodes[index].billboard_mode =
                static_cast<std::uint16_t>(billboard->billboardMode);
        }

        model_.nodes[index].hidden = (obj->flags & 1u) != 0;
        if (auto* shape = dynamic_cast<nifly::NiShape*>(obj)) {
            read_shape(shape, index);
        }
        if (opt_.read_animations) {
            if (auto* system = dynamic_cast<nifly::NiParticleSystem*>(obj)) {
                read_particle_system(system, index);
            }
            controllers_.attach(obj, index);
        }

        if (opt_.read_collision) {
            read_collision(obj, model_.nodes[index].name);
        }

        if (depth >= k_max_node_depth) {
            model_.warnings.push_back("node '" + model_.nodes[index].name +
                                      "': subtree deeper than " +
                                      std::to_string(k_max_node_depth) +
                                      ", children skipped");
            return index;
        }

        if (auto* group = dynamic_cast<nifly::NiNode*>(obj)) {
            const bool ordered = dynamic_cast<nifly::BSOrderedNode*>(obj) != nullptr;
            std::uint32_t order = 0;
            for (auto& ref : group->childRefs) {
                auto* child = nif_.GetHeader().GetBlock<nifly::NiAVObject>(ref);
                if (child == nullptr) {
                    continue;
                }
                if (opt_.skip_editor_markers && is_editor_marker(child->name.get())) {
                    continue;
                }
                // Already visited means a back-reference; following it would
                // loop forever.
                if (node_by_block_.contains(nif_.GetBlockID(child))) {
                    model_.warnings.push_back("node '" + model_.nodes[index].name +
                                              "': child block already visited, cycle broken");
                    continue;
                }
                const std::size_t child_index = walk(child, depth + 1);
                model_.nodes[index].children.push_back(child_index);
                if (ordered) {
                    model_.nodes[child_index].draw_order = order++;
                }
            }
        }
        return index;
    }

    std::size_t add_node_for(nifly::NiShape* shape) {
        const std::size_t index =
            add_node(shape->name.get(), shape->transform, nif_.GetBlockID(shape));
        read_shape(shape, index);
        return index;
    }

    void read_shape(nifly::NiShape* shape, std::size_t node_index) {
        std::vector<nifly::Triangle> tris;
        shape->GetTriangles(tris);
        if (tris.empty() && shape->IsSkinned()) {
            tris = triangles_from_skin_partition(shape);
        }

        const auto* positions = nif_.GetVertsForShape(shape);
        if (positions == nullptr || positions->empty()) {
            if (!opt_.skip_empty_shapes) {
                model_.warnings.push_back(
                    std::format("shape '{}': no vertices", shape->name.get()));
            }
            return;
        }
        if (tris.empty() && opt_.skip_empty_shapes) {
            model_.warnings.push_back(
                std::format("shape '{}': {} vertices but no triangles; skipped",
                            shape->name.get(), positions->size()));
            return;
        }

        Primitive prim;
        prim.name = shape->name.get();
        if (prim.name.empty()) {
            // Blank names are common in vanilla; generate a stable one.
            prim.name = std::format("shape_{}", nif_.GetBlockID(shape));
            model_.nodes[node_index].name = prim.name;
        }

        const std::size_t vcount = positions->size();
        prim.positions.reserve(vcount);
        for (const nifly::Vector3& v : *positions) {
            prim.positions.push_back(to_vec3(v));
        }

        copy_attribute(nif_.GetNormalsForShape(shape), vcount, prim.normals, to_vec3,
                       shape->name.get(), "normals");
        copy_attribute(nif_.GetUvsForShape(shape), vcount, prim.uvs, to_vec2,
                       shape->name.get(), "UVs");
        copy_attribute(nif_.GetColorsForShape(shape), vcount, prim.colors, to_vec4,
                       shape->name.get(), "vertex colours");

        read_tangents(shape, vcount, prim);

        prim.indices.reserve(tris.size() * 3);
        bool out_of_range = false;
        for (const nifly::Triangle& t : tris) {
            if (t.p1 >= vcount || t.p2 >= vcount || t.p3 >= vcount) {
                out_of_range = true;
                continue;
            }
            prim.indices.push_back(t.p1);
            prim.indices.push_back(t.p2);
            prim.indices.push_back(t.p3);
        }
        if (out_of_range) {
            model_.warnings.push_back(std::format(
                "shape '{}': dropped triangles indexing past the vertex array", prim.name));
        }

        if (auto* lod = dynamic_cast<nifly::BSLODTriShape*>(shape)) {
            // LE stores LOD levels in one shape; SE uses separate files.
            prim.lod_level = lod->level0;
        }

        prim.material = read_material(shape);
        if (opt_.read_animations) {
            controllers_.attach(nif_.GetShader(shape), node_index);
            controllers_.attach(nif_.GetAlphaProperty(shape), node_index);
        }

        const std::size_t prim_index = model_.primitives.size();
        model_.primitives.push_back(std::move(prim));
        model_.nodes[node_index].primitives.push_back(prim_index);

        if (opt_.read_skinning && shape->IsSkinned()) {
            pending_skins_.push_back(PendingSkin{prim_index, shape, node_index});
        }
    }

    /// On LE, a skinned NiTriShape may have no triangles of its own; they only
    /// exist in the NiSkinPartition. nifly rebuilds them for BSTriShape but not
    /// NiTriShape, so without this every LE tree and armor piece would have no
    /// surface.
    std::vector<nifly::Triangle> triangles_from_skin_partition(nifly::NiShape* shape) {
        auto* ref = shape->SkinInstanceRef();
        if (ref == nullptr) {
            return {};
        }
        auto* container = nif_.GetHeader().GetBlock<nifly::NiBoneContainer>(ref);
        auto* instance = dynamic_cast<nifly::NiSkinInstance*>(container);
        if (instance == nullptr) {
            return {};
        }
        auto* partition =
            nif_.GetHeader().GetBlock<nifly::NiSkinPartition>(instance->skinPartitionRef);
        if (partition == nullptr) {
            return {};
        }

        // trueTriangles index the shape's vertices; `triangles` index the
        // partition's vertexMap. The wrong one gives a scrambled mesh.
        partition->PrepareTrueTriangles();

        std::vector<nifly::Triangle> out;
        for (const auto& block : partition->partitions) {
            out.insert(out.end(), block.trueTriangles.begin(), block.trueTriangles.end());
        }
        if (!out.empty()) {
            model_.warnings.push_back(std::format(
                "shape '{}': triangles recovered from the skin partition; the shape "
                "itself carried none",
                shape->name.get()));
        }
        return out;
    }

    /// Copy an optional per-vertex attribute, dropping it if its length does
    /// not match the vertex count.
    template <typename Src, typename Dst, typename Fn>
    void copy_attribute(const std::vector<Src>* src, std::size_t vcount,
                        std::vector<Dst>& dst, Fn convert, const std::string& shape_name,
                        std::string_view what) {
        if (src == nullptr || src->empty()) {
            return;
        }
        if (src->size() != vcount) {
            model_.warnings.push_back(
                std::format("shape '{}': {} count {} != vertex count {}; dropped",
                            shape_name, what, src->size(), vcount));
            return;
        }
        dst.reserve(vcount);
        for (const Src& v : *src) {
            dst.push_back(convert(v));
        }
    }

    /// glTF TANGENT is xyzw with w as handedness. Bethesda's two arrays are
    /// named the other way round: nifly's "bitangents" follow U and its
    /// "tangents" follow V (measured against UV derivatives, LE and SE alike).
    /// The NIF's V vector points down the image, as its DirectX-style normal
    /// maps expect; glTF's bitangent (normal x tangent, times w) points up it,
    /// so w is the opposite of the NIF's handedness. Consumers still flip the
    /// normal maps' green channel.
    void read_tangents(nifly::NiShape* shape, std::size_t vcount, Primitive& prim) {
        const auto* along_v = nif_.GetTangentsForShape(shape);
        const auto* along_u = nif_.GetBitangentsForShape(shape);
        if (along_u == nullptr || along_u->size() != vcount || along_v == nullptr ||
            along_v->size() != vcount) {
            return;
        }
        if (prim.normals.empty()) {
            // glTF forbids TANGENT without NORMAL.
            model_.warnings.push_back(std::format(
                "shape '{}': tangents present but normals are not; tangents dropped",
                prim.name));
            return;
        }
        prim.tangents.reserve(vcount);
        for (std::size_t i = 0; i < vcount; ++i) {
            const Vec3& n = prim.normals[i];
            const nifly::Vector3& t = (*along_u)[i];
            const nifly::Vector3& b = (*along_v)[i];
            const float cx = n.y * t.z - n.z * t.y;
            const float cy = n.z * t.x - n.x * t.z;
            const float cz = n.x * t.y - n.y * t.x;
            const float sign = (cx * b.x + cy * b.y + cz * b.z) < 0.0f ? 1.0f : -1.0f;
            prim.tangents.push_back(Vec4{t.x, t.y, t.z, sign});
        }
    }

    std::size_t read_material(nifly::NiShape* shape) {
        TextureSlots slots{};
        for (std::uint32_t slot = 0; slot < slots.size(); ++slot) {
            nif_.GetTextureSlot(shape, slots[slot], slot);
        }
        // Sky shaders (stars, sky domes) name their one texture themselves.
        if (auto* sky = dynamic_cast<nifly::BSSkyShaderProperty*>(nif_.GetShader(shape));
            sky != nullptr && slots[0].empty()) {
            slots[0] = sky->baseTexture.get();
        }
        return read_material(shape->name.get(), nif_.GetShader(shape),
                             nif_.GetAlphaProperty(shape), std::move(slots));
    }

    /// Particle systems carry their properties as refs of their own, and
    /// their effect shader keeps texture paths in its own fields.
    void read_particle_system(nifly::NiParticleSystem* system, std::size_t node) {
        auto& hdr = nif_.GetHeader();
        auto* shader = hdr.GetBlock<nifly::NiShader>(system->shaderPropertyRef.index);
        auto* alpha = hdr.GetBlock<nifly::NiAlphaProperty>(system->alphaPropertyRef.index);
        TextureSlots slots{};
        if (auto* effect = dynamic_cast<nifly::BSEffectShaderProperty*>(shader)) {
            slots[0] = effect->sourceTexture.get();
            slots[1] = effect->normalTexture.get();
            slots[3] = effect->greyscaleTexture.get();
            slots[4] = effect->envMapTexture.get();
            slots[5] = effect->envMaskTexture.get();
        } else if (shader != nullptr) {
            if (auto* set = hdr.GetBlock(shader->TextureSetRef())) {
                for (std::size_t i = 0; i < slots.size() && i < set->textures.size(); ++i) {
                    slots[i] = set->textures[static_cast<std::uint32_t>(i)].get();
                }
            }
        }
        const std::size_t material =
            read_material(system->name.get(), shader, alpha, std::move(slots));
        controllers_.add_particles(system, node, material);
        controllers_.attach(shader, node);
        controllers_.attach(alpha, node);
    }

    std::size_t read_material(const std::string& name, nifly::NiShader* shader,
                              nifly::NiAlphaProperty* alpha, TextureSlots slots) {
        Material mat;
        mat.name = name;
        if (mat.name.empty()) {
            mat.name = std::format("material_{}", model_.materials.size());
        }
        mat.kind = kind_of(shader);
        if (shader != nullptr) {
            mat.bs_shader_type = shader->GetShaderType();
            mat.glossiness = shader->GetGlossiness();
            mat.specular_strength = shader->GetSpecularStrength();
            mat.specular_color = to_vec3(shader->GetSpecularColor());
            mat.uv_offset = to_vec2(shader->GetUVOffset());
            mat.uv_scale = to_vec2(shader->GetUVScale());
            mat.environment_map_scale = shader->GetEnvironmentMapScale();
            mat.double_sided = shader->IsDoubleSided();
            mat.model_space_normals = shader->IsModelSpace();
            mat.skin_tinted = shader->IsSkinTinted();
            mat.face_tinted = shader->IsFaceTinted();
            mat.has_glowmap = shader->HasGlowmap();
            mat.has_environment_map = shader->HasEnvironmentMapping();
            mat.has_backlight = shader->HasBacklight();
            mat.has_rimlight = shader->HasRimlight();
            mat.has_softlight = shader->HasSoftlight();

            mat.base_color.w = shader->GetAlpha();

            if (const auto* ls = dynamic_cast<nifly::BSLightingShaderProperty*>(shader)) {
                if (mat.bs_shader_type == nifly::BSLSP_HAIRTINT) {
                    mat.hair_tint = to_vec3(ls->hairTintColor);
                } else if (mat.bs_shader_type == nifly::BSLSP_SKINTINT) {
                    mat.skin_tint = to_vec3(ls->skinTintColor);
                }
            }
            if (const auto* bs = dynamic_cast<nifly::BSShaderProperty*>(shader)) {
                mat.shader_flags1 = bs->shaderFlags1;
                mat.shader_flags2 = bs->shaderFlags2;
                mat.refraction = mat.kind == ShaderKind::lighting &&
                                 (bs->shaderFlags1 & nifly::SLSF1_REFRACTION) != 0;
            }

            // IsEmissive() is set on almost every vanilla material; use the
            // emissive color instead (black means no emission).
            const nifly::Color4 emit = shader->GetEmissiveColor();
            const float multiple = shader->GetEmissiveMultiple();
            mat.emissive = Vec3{emit.r, emit.g, emit.b};
            mat.emissive_alpha = emit.a;
            if (const auto* effect = dynamic_cast<nifly::BSEffectShaderProperty*>(shader)) {
                mat.falloff = Vec4{effect->falloffStartAngle, effect->falloffStopAngle,
                                   effect->falloffStartOpacity, effect->falloffStopOpacity};
                mat.soft_falloff_depth = effect->softFalloffDepth;
            }
            mat.emissive_strength = multiple > 0.0f ? multiple : 1.0f;
            mat.textures = std::move(slots);
        }

        mat.alpha_property_present = alpha != nullptr;
        if (alpha != nullptr) {
            mat.alpha_flags = alpha->flags;
        }
        mat.alpha_mode = alpha_mode_of(alpha, &mat.alpha_cutoff);
        if (mat.alpha_mode == AlphaMode::opaque && mat.base_color.w < 1.0f) {
            // Shader alpha < 1 without NiAlphaProperty (vanilla glass and ice)
            // needs blending or it renders opaque.
            mat.alpha_mode = AlphaMode::blend;
        }

        model_.materials.push_back(std::move(mat));
        return model_.materials.size() - 1;
    }

    void read_collision(nifly::NiAVObject* obj, const std::string& node_name) {
        auto* collision =
            nif_.GetHeader().GetBlock<nifly::bhkNiCollisionObject>(obj->collisionRef);
        if (collision == nullptr) {
            return;
        }
        auto* body = nif_.GetHeader().GetBlock<nifly::bhkRigidBody>(collision->bodyRef);
        if (body == nullptr) {
            return;
        }
        auto* shape = nif_.GetHeader().GetBlock<nifly::bhkShape>(body->shapeRef);
        if (shape == nullptr) {
            return;
        }

        CollisionContext ctx;
        ctx.node_name = node_name;
        ctx.layer = body->collisionFilter.layer;
        ctx.motion_type = body->motionSystem;
        ctx.quality_type = body->qualityType;
        ctx.mass = body->mass;
        ctx.friction = body->friction;
        ctx.restitution = body->restitution;
        // bhkRigidBodyT has its own transform; bhkRigidBody uses the node's.
        if (dynamic_cast<nifly::bhkRigidBodyT*>(body) != nullptr) {
            ctx.transform.translation =
                nifly::Vector3(body->translation.x, body->translation.y, body->translation.z);
            ctx.transform.rotation = quat_to_matrix(body->rotation);
        }
        read_collision_shape(shape, ctx, 0);
    }

    struct CollisionContext {
        std::string node_name;
        std::uint8_t layer{};
        std::uint8_t motion_type{};
        std::uint8_t quality_type{};
        float mass{};
        float friction{};
        float restitution{};
        nifly::MatTransform transform; ///< Body, then each transform shape.
    };

    void read_collision_shape(nifly::bhkShape* shape, const CollisionContext& ctx,
                              int depth) {
        // bhkListShape can nest, even into itself; bound the depth.
        if (shape == nullptr || depth > 8) {
            return;
        }

        CollisionShape out;
        out.block_name = shape->GetBlockName();
        out.target_node = ctx.node_name;
        out.havok_material = shape->GetMaterial();
        out.layer = ctx.layer;
        out.motion_type = ctx.motion_type;
        out.quality_type = ctx.quality_type;
        out.mass = ctx.mass;
        out.friction = ctx.friction;
        out.restitution = ctx.restitution;
        out.transform = to_transform(ctx.transform);

        if (auto* mopp = dynamic_cast<nifly::bhkMoppBvTreeShape*>(shape)) {
            // MOPP is an acceleration tree around the real shape; unwrap it.
            read_collision_shape(nif_.GetHeader().GetBlock<nifly::bhkShape>(mopp->shapeRef),
                                 ctx, depth + 1);
            return;
        }
        if (auto* list = dynamic_cast<nifly::bhkListShape*>(shape)) {
            for (auto& ref : list->subShapeRefs) {
                read_collision_shape(nif_.GetHeader().GetBlock<nifly::bhkShape>(ref), ctx,
                                     depth + 1);
            }
            return;
        }
        if (auto* xform = dynamic_cast<nifly::bhkTransformShape*>(shape)) {
            CollisionContext nested = ctx;
            nested.transform = ctx.transform.ComposeTransforms(mat_transform_of(xform->xform));
            read_collision_shape(nif_.GetHeader().GetBlock<nifly::bhkShape>(xform->shapeRef),
                                 nested, depth + 1);
            return;
        }

        if (auto* box = dynamic_cast<nifly::bhkBoxShape*>(shape)) {
            out.kind = CollisionKind::box;
            out.half_extents = to_vec3(box->dimensions);
            out.radius = box->radius;
        } else if (auto* capsule = dynamic_cast<nifly::bhkCapsuleShape*>(shape)) {
            out.kind = CollisionKind::capsule;
            out.point_a = to_vec3(capsule->point1);
            out.point_b = to_vec3(capsule->point2);
            out.radius = capsule->radius1;
        } else if (auto* sphere = dynamic_cast<nifly::bhkSphereShape*>(shape)) {
            out.kind = CollisionKind::sphere;
            out.radius = sphere->radius;
        } else if (auto* convex = dynamic_cast<nifly::bhkConvexVerticesShape*>(shape)) {
            out.kind = CollisionKind::convex_vertices;
            out.radius = convex->radius;
            out.vertices.reserve(convex->verts.size());
            for (const nifly::Vector4& v : convex->verts) {
                out.vertices.push_back(Vec3{v.x, v.y, v.z});
            }
        } else if (auto* mesh = dynamic_cast<nifly::bhkCompressedMeshShape*>(shape)) {
            out.kind = CollisionKind::compressed_mesh;
            read_compressed_mesh(mesh, out);
        } else if (auto* cylinder = dynamic_cast<nifly::bhkCylinderShape*>(shape)) {
            read_cylinder(*cylinder, out);
        } else if (auto* strips = dynamic_cast<nifly::bhkNiTriStripsShape*>(shape)) {
            out.kind = CollisionKind::mesh;
            read_strips(*strips, out);
        } else if (auto* plane = dynamic_cast<nifly::bhkPlaneShape*>(shape)) {
            read_plane(*plane, out);
            if (out.kind == CollisionKind::unsupported) {
                model_.warnings.push_back(std::format(
                    "collision on '{}': plane does not cut its bounds", ctx.node_name));
            }
        } else {
            model_.warnings.push_back(
                std::format("collision on '{}': unsupported Havok shape '{}'",
                            ctx.node_name, out.block_name));
        }

        model_.collision.push_back(std::move(out));
    }

    /// The convex radius pads a Havok cylinder all round, as it does a box:
    /// folded into the radius and the end points.
    static void read_cylinder(const nifly::bhkCylinderShape& cylinder, CollisionShape& out) {
        out.kind = CollisionKind::cylinder;
        const nifly::Vector3 a(cylinder.vertexA.x, cylinder.vertexA.y, cylinder.vertexA.z);
        const nifly::Vector3 b(cylinder.vertexB.x, cylinder.vertexB.y, cylinder.vertexB.z);
        nifly::Vector3 axis = b - a;
        const float length = axis.length();
        if (length > 0.0f) {
            axis /= length;
        }
        const float pad = std::max(cylinder.radius, 0.0f);
        out.point_a = to_vec3(a - axis * pad);
        out.point_b = to_vec3(b + axis * pad);
        out.radius = cylinder.cylinderRadius + pad;
    }

    /// NiTriStripsData parts, in game units, divided down to Havok units like
    /// every other shape.
    void read_strips(nifly::bhkNiTriStripsShape& strips, CollisionShape& out) {
        for (auto& ref : strips.partRefs) {
            const auto* data = nif_.GetHeader().GetBlock<nifly::NiTriStripsData>(ref);
            if (data == nullptr) {
                continue;
            }
            const auto base = static_cast<std::uint32_t>(out.vertices.size());
            for (const nifly::Vector3& v : data->vertices) {
                out.vertices.push_back(Vec3{v.x / k_havok_scale, v.y / k_havok_scale,
                                            v.z / k_havok_scale});
            }
            std::vector<nifly::Triangle> triangles;
            data->GetTriangles(triangles);
            for (const nifly::Triangle& t : triangles) {
                if (t.p1 >= data->vertices.size() || t.p2 >= data->vertices.size() ||
                    t.p3 >= data->vertices.size()) {
                    continue;
                }
                out.indices.push_back(base + t.p1);
                out.indices.push_back(base + t.p2);
                out.indices.push_back(base + t.p3);
            }
        }
    }

    /// bhkPlaneShape: a plane bounded by a box (centre, half extents). Kept as
    /// the flat hull where the plane cuts the box; consumers thicken flat
    /// hulls as they do Havok's thin convex shapes.
    static void read_plane(const nifly::bhkPlaneShape& plane, CollisionShape& out) {
        const nifly::Vector3 n = plane.plane.normal;
        const nifly::Vector3 c(plane.center.x, plane.center.y, plane.center.z);
        const nifly::Vector3 h(plane.halfExtents.x, plane.halfExtents.y, plane.halfExtents.z);
        std::array<nifly::Vector3, 8> corners;
        for (std::size_t i = 0; i < 8; ++i) {
            corners[i] = c + nifly::Vector3((i & 1) != 0 ? h.x : -h.x, (i & 2) != 0 ? h.y : -h.y,
                                            (i & 4) != 0 ? h.z : -h.z);
        }
        // Havok stores the plane as n.p + w = 0.
        const auto side = [&](const nifly::Vector3& p) { return n.dot(p) + plane.plane.constant; };
        for (std::size_t i = 0; i < 8; ++i) {
            for (std::size_t bit = 1; bit < 8; bit <<= 1) {
                const std::size_t j = i | bit;
                if (j == i) {
                    continue;
                }
                const float si = side(corners[i]);
                const float sj = side(corners[j]);
                if ((si <= 0.0f) != (sj <= 0.0f) || si == 0.0f) {
                    const float t = si == sj ? 0.0f : si / (si - sj);
                    out.vertices.push_back(to_vec3(corners[i] + (corners[j] - corners[i]) * t));
                }
            }
        }
        out.kind = out.vertices.size() >= 3 ? CollisionKind::convex_vertices
                                            : CollisionKind::unsupported;
    }

    static nifly::MatTransform mat_transform_of(const nifly::Matrix4& m) {
        nifly::MatTransform t;
        t.rotation = nifly::Matrix3(m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
        t.translation = nifly::Vector3(m[3], m[7], m[11]);
        return t;
    }

    /// bhkCompressedMeshShapeData stores chunk-local uint16 vertices, scaled
    /// against the chunk translation and placed by an optional per-chunk
    /// transform. The 1/1000 factor is undocumented, so results are checked
    /// against the stored AABB.
    void read_compressed_mesh(nifly::bhkCompressedMeshShape* mesh, CollisionShape& out) {
        auto* data =
            nif_.GetHeader().GetBlock<nifly::bhkCompressedMeshShapeData>(mesh->dataRef);
        if (data == nullptr) {
            model_.warnings.push_back(
                std::format("collision on '{}': compressed mesh has no data block",
                            out.target_node));
            return;
        }

        constexpr float kQuantum = 1.0f / 1000.0f;

        for (nifly::Vector4& v : data->bigVerts) {
            out.vertices.push_back(Vec3{v.x, v.y, v.z});
        }
        for (nifly::bhkCMSDBigTris& t : data->bigTris) {
            out.indices.push_back(t.triangle1);
            out.indices.push_back(t.triangle2);
            out.indices.push_back(t.triangle3);
        }

        // nifly's NiVector only has a non-const uint32 subscript; copy each
        // chunk's arrays once.
        std::vector<nifly::bhkCMSDTransform> transforms(data->transforms.begin(),
                                                        data->transforms.end());

        for (nifly::bhkCMSDChunk& chunk : data->chunks) {
            const std::vector<std::uint16_t> verts(chunk.verts.begin(), chunk.verts.end());
            const std::vector<std::uint16_t> indices(chunk.indices.begin(),
                                                     chunk.indices.end());
            const std::vector<std::uint16_t> strips(chunk.strips.begin(),
                                                    chunk.strips.end());

            const std::size_t base = out.vertices.size();
            const bool has_transform = chunk.transformIndex < transforms.size();
            nifly::MatTransform placement;
            if (has_transform) {
                const nifly::bhkCMSDTransform& xf = transforms[chunk.transformIndex];
                placement.translation =
                    nifly::Vector3(xf.translation.x, xf.translation.y, xf.translation.z);
                placement.rotation = quat_to_matrix(xf.rotation);
            }

            for (std::size_t i = 0; i + 2 < verts.size(); i += 3) {
                Vec3 p{chunk.translation.x + static_cast<float>(verts[i]) * kQuantum,
                       chunk.translation.y + static_cast<float>(verts[i + 1]) * kQuantum,
                       chunk.translation.z + static_cast<float>(verts[i + 2]) * kQuantum};
                if (has_transform) {
                    p = to_vec3(placement.ApplyTransform(nifly::Vector3(p.x, p.y, p.z)));
                }
                out.vertices.push_back(p);
            }

            // Chunk indices start with the strips `strips` lists the lengths
            // of; whatever follows them is a plain triangle list. Chunks
            // with both are common (vanilla's Dragonsreach stairs lose their
            // bridge without the list).
            const std::size_t chunk_vcount = out.vertices.size() - base;
            std::size_t offset = 0;
            for (std::uint16_t strip_len : strips) {
                for (std::size_t i = 0; i + 2 < strip_len; ++i) {
                    const std::size_t k = offset + i;
                    if (k + 2 >= indices.size()) {
                        break;
                    }
                    // Keep strip winding so normals do not alternate.
                    if ((i % 2) == 0) {
                        push_triangle(out, base, chunk_vcount, indices[k], indices[k + 1],
                                      indices[k + 2]);
                    } else {
                        push_triangle(out, base, chunk_vcount, indices[k + 1], indices[k],
                                      indices[k + 2]);
                    }
                }
                offset += strip_len;
            }
            for (std::size_t i = offset; i + 2 < indices.size(); i += 3) {
                push_triangle(out, base, chunk_vcount, indices[i], indices[i + 1],
                              indices[i + 2]);
            }
        }

        check_against_stored_aabb(data, out);
    }

    static void push_triangle(CollisionShape& out, std::size_t base, std::size_t count,
                              std::uint16_t a, std::uint16_t b, std::uint16_t c) {
        if (a >= count || b >= count || c >= count) {
            return;
        }
        out.indices.push_back(static_cast<std::uint32_t>(base + a));
        out.indices.push_back(static_cast<std::uint32_t>(base + b));
        out.indices.push_back(static_cast<std::uint32_t>(base + c));
    }

    static nifly::Matrix3 quat_to_matrix(const nifly::QuaternionXYZW& q) {
        const float x = q.x, y = q.y, z = q.z, w = q.w;
        return nifly::Matrix3(1 - 2 * (y * y + z * z), 2 * (x * y - z * w),
                              2 * (x * z + y * w), 2 * (x * y + z * w),
                              1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                              2 * (x * z - y * w), 2 * (y * z + x * w),
                              1 - 2 * (x * x + y * y));
    }

    /// Warn if decoded vertices fall outside the AABB stored in the data block,
    /// which would mean the dequantization factor is wrong.
    void check_against_stored_aabb(const nifly::bhkCompressedMeshShapeData* data,
                                   const CollisionShape& out) {
        if (out.vertices.empty()) {
            return;
        }
        const float slack = 0.05f;
        const nifly::Vector4& lo = data->aabbBoundMin;
        const nifly::Vector4& hi = data->aabbBoundMax;
        if (lo.x >= hi.x && lo.y >= hi.y && lo.z >= hi.z) {
            return; // No usable AABB stored.
        }
        std::size_t outside = 0;
        for (const Vec3& v : out.vertices) {
            if (v.x < lo.x - slack || v.x > hi.x + slack || v.y < lo.y - slack ||
                v.y > hi.y + slack || v.z < lo.z - slack || v.z > hi.z + slack) {
                ++outside;
            }
        }
        if (outside != 0) {
            model_.warnings.push_back(std::format(
                "collision on '{}': {} of {} decoded compressed-mesh vertices fall "
                "outside the AABB the file states for itself",
                out.target_node, outside, out.vertices.size()));
        }
    }

    /// Resolve skins after the whole graph exists, since bones may be walked
    /// later than the shape.
    void resolve_skins() {
        for (const PendingSkin& pending : pending_skins_) {
            std::vector<std::string> bone_names;
            nif_.GetShapeBoneList(pending.shape, bone_names);
            if (bone_names.empty()) {
                model_.warnings.push_back(
                    std::format("shape '{}': marked skinned but has no bones",
                                model_.primitives[pending.primitive].name));
                continue;
            }

            Skin skin;
            skin.name = model_.primitives[pending.primitive].name + "_skin";

            // glTF inverse bind matrices map mesh space to joint bind space. NIF
            // gives global-to-skin per shape and skin-to-bone per bone; combined
            // with the shape's placement they give that matrix.
            nifly::MatTransform global_to_skin;
            nif_.CalcShapeTransformGlobalToSkin(pending.shape, global_to_skin);
            // Walk the shape's own parents: nifly's GetNodeTransformToGlobal
            // only finds NiNodes, so a shape's placement (a body sits 120
            // units up) would silently drop out. Bounded like walk(), since a
            // cyclic graph has no top.
            nifly::MatTransform shape_to_global = pending.shape->GetTransformToParent();
            std::size_t depth = 0;
            for (nifly::NiNode* parent = nif_.GetParentNode(pending.shape);
                 parent != nullptr && depth < k_max_node_depth;
                 parent = nif_.GetParentNode(parent), ++depth) {
                shape_to_global = parent->GetTransformToParent().ComposeTransforms(shape_to_global);
            }
            const nifly::MatTransform shape_to_skin =
                global_to_skin.ComposeTransforms(shape_to_global);

            std::vector<std::size_t> bone_nodes;
            bone_nodes.reserve(bone_names.size());
            std::size_t missing = 0;
            for (std::uint32_t bone = 0; bone < bone_names.size(); ++bone) {
                const auto found = node_by_name_.find(bone_names[bone]);
                if (found == node_by_name_.end()) {
                    // Armor names skeleton bones the mesh lacks. Create a node
                    // so joint indices stay dense and the engine can rebind.
                    ++missing;
                    nifly::MatTransform identity;
                    const std::size_t created =
                        add_node(bone_names[bone], identity, ~0u - bone);
                    model_.nodes[created].is_joint = true;
                    model_.roots.push_back(created);
                    bone_nodes.push_back(created);
                } else {
                    model_.nodes[found->second].is_joint = true;
                    bone_nodes.push_back(found->second);
                }

                nifly::MatTransform skin_to_bone;
                nif_.GetShapeTransformSkinToBone(pending.shape, bone, skin_to_bone);
                skin.inverse_bind_matrices.push_back(
                    to_mat4(skin_to_bone.ComposeTransforms(shape_to_skin)));
            }
            if (missing != 0) {
                model_.warnings.push_back(std::format(
                    "shape '{}': {} of {} skin bones are not nodes in this file; "
                    "placeholder joints emitted",
                    model_.primitives[pending.primitive].name, missing, bone_names.size()));
            }
            skin.joints = std::move(bone_nodes);

            read_skin_weights(pending, skin.joints.size());

            model_.skins.push_back(std::move(skin));
            model_.primitives[pending.primitive].skin = model_.skins.size() - 1;
        }
    }

    /// One bone's weights by vertex. nifly's GetShapeBoneWeights binds a
    /// reference to the float in its packed SkinWeight for NiSkinData
    /// (misaligned: UBSan stops there), so that case is read here by value;
    /// BSTriShape keeps its weights in the vertex data and goes through nifly.
    void bone_weights(nifly::NiShape* shape, std::uint32_t bone,
                      std::unordered_map<std::uint16_t, float>& out) const {
        out.clear();
        if (dynamic_cast<nifly::BSTriShape*>(shape) != nullptr) {
            nif_.GetShapeBoneWeights(shape, bone, out);
            return;
        }
        const auto& hdr = nif_.GetHeader();
        const auto* instance = hdr.GetBlock<nifly::NiSkinInstance>(shape->SkinInstanceRef());
        const auto* data = instance != nullptr ? hdr.GetBlock(instance->dataRef) : nullptr;
        if (data == nullptr || bone >= data->numBones || bone >= data->bones.size()) {
            return;
        }
        for (const auto& sw : data->bones[bone].vertexWeights) {
            const std::uint16_t index = sw.index;
            const float weight = sw.weight;
            if (weight >= nifly::EPSILON) {
                out.emplace(index, weight);
            }
        }
    }

    /// NIF weights are per bone (vertex -> weight); glTF wants four per vertex.
    /// The four-influence limit is enforced and counted here.
    void read_skin_weights(const PendingSkin& pending, std::size_t bone_count) {
        Primitive& prim = model_.primitives[pending.primitive];
        const std::size_t vcount = prim.positions.size();
        prim.joints.assign(vcount, {0, 0, 0, 0});
        prim.weights.assign(vcount, Vec4{0, 0, 0, 0});

        std::vector<std::uint8_t> influences(vcount, 0);
        std::size_t over_limit = 0;

        for (std::uint32_t bone = 0; bone < bone_count; ++bone) {
            std::unordered_map<std::uint16_t, float> weights;
            bone_weights(pending.shape, bone, weights);
            for (const auto& [vertex, weight] : weights) {
                if (vertex >= vcount || weight <= 0.0f) {
                    continue;
                }
                std::uint8_t& slot = influences[vertex];
                if (slot >= 4) {
                    ++over_limit;
                    continue;
                }
                prim.joints[vertex][slot] = static_cast<std::uint16_t>(bone);
                (&prim.weights[vertex].x)[slot] = weight;
                ++slot;
            }
        }

        // glTF requires weights summing to 1; renormalize.
        for (std::size_t v = 0; v < vcount; ++v) {
            Vec4& w = prim.weights[v];
            const float sum = w.x + w.y + w.z + w.w;
            if (sum > 0.0f) {
                w.x /= sum;
                w.y /= sum;
                w.z /= sum;
                w.w /= sum;
            } else {
                // No weights: bind rigidly to the first joint, as NIF does,
                // instead of collapsing to the origin.
                w.x = 1.0f;
            }
        }

        if (over_limit != 0) {
            model_.warnings.push_back(std::format(
                "shape '{}': {} bone influences past the fourth were dropped",
                prim.name, over_limit));
        }
    }
};

} // namespace

std::string_view to_string(NifFlavor flavor) noexcept {
    switch (flavor) {
    case NifFlavor::le:
        return "LE";
    case NifFlavor::se:
        return "SE";
    case NifFlavor::unknown:
        break;
    }
    return "unknown";
}

io::ParseResult<Model> read_nif(std::span<const std::byte> bytes, std::string_view origin,
                                const ReadOptions& options) {
    if (bytes.empty()) {
        return std::unexpected(fail(origin, ErrorKind::truncated, "empty file"));
    }

    Model model;
    model.source = std::string(origin);

    // From here nifly may throw: it allocates from sizes read from the file.
    try {
        nifly::NifFile nif;
        io::SpanStream stream(bytes);
        const int rc = nif.Load(stream);
        if (rc != 0 || !nif.IsValid()) {
            return std::unexpected(fail(origin, ErrorKind::bad_magic,
                                        std::format("nifly rejected the file (code {})", rc)));
        }

        const nifly::NiVersion version = nif.GetHeader().GetVersion();
        model.nif_version = version.String();
        model.nif_stream = version.Stream();

        if (nif.HasUnknown()) {
            // Some block types were skipped. Geometry is usually fine, but a
            // skipped block may be collision or shader, so warn.
            model.warnings.emplace_back(
                "file contains block types nifly does not know; some data was skipped");
        }

        Reader reader(nif, model, options);
        reader.run();
    } catch (const std::exception& e) {
        return std::unexpected(
            fail(origin, ErrorKind::corrupt, std::format("nifly threw: {}", e.what())));
    } catch (...) {
        return std::unexpected(
            fail(origin, ErrorKind::corrupt, "nifly threw a non-std exception"));
    }

    return model;
}

} // namespace bethconv::mesh
