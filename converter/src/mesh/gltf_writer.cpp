// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/mesh/gltf_writer.hpp"

#include "bethconv/io/byte_writer.hpp"
#include "bethconv/io/json_text.hpp"
#include "bethconv/io/span_stream.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <limits>
#include <map>
#include <numbers>

namespace bethconv::mesh {
namespace {

using io::ErrorKind;
using io::ParseError;

ParseError fail(const Model& model, ErrorKind kind, std::string detail) {
    return ParseError{model.source, 0, kind, std::move(detail)};
}

/// Backslashes to slashes, lowercase.
std::string to_uri(std::string_view bethesda_path) {
    std::string out;
    out.reserve(bethesda_path.size());
    for (const char c : bethesda_path) {
        out.push_back(c == '\\' ? '/' : static_cast<char>(std::tolower(
                                            static_cast<unsigned char>(c))));
    }
    return out;
}

using bethconv::io::json_text;

constexpr char k_hex[] = "0123456789ABCDEF";

void append_escaped(std::string& out, unsigned char c) {
    out.push_back('%');
    out.push_back(k_hex[c >> 4]);
    out.push_back(k_hex[c & 0x0F]);
}

/// Make a texture path a URI reference (RFC 3986 §4.2): escape everything
/// outside `pchar` except `/` (spaces occur in 49 vanilla paths; `#` and `?`
/// would start a fragment or query). `:` is escaped too, because a colon in the
/// first segment of a relative reference reads as a scheme.
std::string uri_escape(std::string_view path) {
    static constexpr std::string_view k_extra = "!$&'()*+,;=@";
    std::string out;
    out.reserve(path.size());
    for (const char raw : path) {
        const auto c = static_cast<unsigned char>(raw);
        const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                                c == '_' || c == '~';
        if (unreserved || c == '/' || k_extra.find(static_cast<char>(c)) !=
                                          std::string_view::npos) {
            out.push_back(raw);
        } else {
            append_escaped(out, c);
        }
    }
    return out;
}

/// Double every `%` before building a `fastgltf::URI`.
///
/// `fastgltf::URI` percent-decodes in its constructor and the exporter writes
/// the decoded form, so `textures/%08nor` would be emitted with a raw 0x08.
/// `%2508` decodes to `%08`, which is what the document should contain. It also
/// avoids fastgltf 0.9.0 reading past the string on a trailing `%` (found by
/// fuzz_nif): every `%` it sees is followed by two hex digits.
std::string shield_percents(std::string_view uri) {
    std::string out;
    out.reserve(uri.size());
    for (const char c : uri) {
        if (c == '%') {
            out += "%25";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// Filter for every float written as a JSON number.
///
/// JSON has no NaN or infinity, and fastgltf 0.9.0 writes them as out-of-range
/// numbers (NaN -> `2.696539702293474e+308`) without an error. Vanilla SE has
/// one (see nif_reader.cpp). Non-finite values become 0; clamping would produce
/// a coordinate 10^308 units away.
///
/// Binary buffer data is not filtered: NaN is representable there and buffers
/// keep the NIF's values.
[[nodiscard]] float json_number(float value) noexcept {
    return std::isfinite(value) ? value : 0.0f;
}

/// The `../` prefix that makes a pack-relative texture path resolve from the
/// GLB's directory. glTF resolves URIs against the document, and meshes and
/// textures mirror the virtual filesystem, so the prefix is the source path's
/// depth. An empty `source` gets no prefix.
std::string ascent_to_root(std::string_view source) {
    std::size_t depth = 0;
    for (const char c : source) {
        if (c == '/' || c == '\\') {
            ++depth;
        }
    }
    std::string out;
    out.reserve(depth * 3);
    for (std::size_t i = 0; i < depth; ++i) {
        out += "../";
    }
    return out;
}

/// Names for the nine texture slots, for `extras` only. Only slots 0 and 1 have
/// a fixed meaning and map to glTF; the others depend on the shader type.
std::string_view slot_name(std::size_t slot, ShaderKind kind, std::uint32_t shader_type) {
    if (kind == ShaderKind::effect) {
        switch (slot) {
        case 0: return "source";
        case 1: return "greyscale";
        case 3: return "cube";
        case 4: return "env_mask";
        default: return "unused";
        }
    }
    switch (slot) {
    case 0: return "diffuse";
    case 1: return "normal";
    case 2:
        // Shader type 5 (skin tint): slot 2 is the subsurface tint map, not glow.
        return shader_type == 5 ? "subsurface_tint" : "glow";
    case 3: return "detail_or_height";
    case 4: return "environment";
    case 5: return "environment_mask";
    case 6: return shader_type == 5 ? "inner_layer" : "multilayer";
    case 7: return shader_type == 5 ? "specular" : "backlight";
    case 8: return "unused";
    default: return "unused";
    }
}

fastgltf::AlphaMode to_gltf_alpha(AlphaMode mode) {
    switch (mode) {
    case AlphaMode::mask:
        return fastgltf::AlphaMode::Mask;
    case AlphaMode::blend:
        return fastgltf::AlphaMode::Blend;
    case AlphaMode::opaque:
        break;
    }
    return fastgltf::AlphaMode::Opaque;
}

std::string_view collision_kind_name(CollisionKind kind) {
    switch (kind) {
    case CollisionKind::box: return "box";
    case CollisionKind::sphere: return "sphere";
    case CollisionKind::capsule: return "capsule";
    case CollisionKind::convex_vertices: return "convex_vertices";
    case CollisionKind::compressed_mesh: return "compressed_mesh";
    case CollisionKind::list: return "list";
    case CollisionKind::cylinder: return "cylinder";
    case CollisionKind::mesh: return "mesh";
    case CollisionKind::unsupported: break;
    }
    return "unsupported";
}

/// Builds the single glTF buffer and allocates accessors, each aligned to its
/// component size (JOINTS_0 u16 and POSITION f32 share the buffer).
class BufferBuilder {
public:
    explicit BufferBuilder(fastgltf::Asset& asset) : asset_(asset) {}

    template <typename T>
    std::size_t add(const std::vector<T>& values, fastgltf::AccessorType type,
                    fastgltf::ComponentType component, bool is_index_data,
                    std::size_t components_per_element) {
        const std::size_t component_size = fastgltf::getComponentByteSize(component);
        bytes_.align_to(component_size);
        const std::size_t offset = bytes_.size();
        bytes_.put_all(std::span<const T>(values));
        const std::size_t length = bytes_.size() - offset;

        fastgltf::BufferView view{};
        view.bufferIndex = 0;
        view.byteOffset = offset;
        view.byteLength = length;
        view.target = is_index_data ? fastgltf::BufferTarget::ElementArrayBuffer
                                    : fastgltf::BufferTarget::ArrayBuffer;
        asset_.bufferViews.push_back(std::move(view));

        fastgltf::Accessor accessor{};
        accessor.bufferViewIndex = asset_.bufferViews.size() - 1;
        accessor.byteOffset = 0;
        accessor.count = length / (component_size * components_per_element);
        accessor.type = type;
        accessor.componentType = component;
        asset_.accessors.push_back(std::move(accessor));
        return asset_.accessors.size() - 1;
    }

    /// glTF requires min/max on POSITION; loaders use it for bounds.
    void set_position_bounds(std::size_t accessor, const std::vector<Vec3>& positions) {
        if (positions.empty()) {
            return;
        }
        float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max()};
        float hi[3] = {std::numeric_limits<float>::lowest(),
                       std::numeric_limits<float>::lowest(),
                       std::numeric_limits<float>::lowest()};
        for (const Vec3& p : positions) {
            const float v[3] = {p.x, p.y, p.z};
            for (int i = 0; i < 3; ++i) {
                lo[i] = std::min(lo[i], v[i]);
                hi[i] = std::max(hi[i], v[i]);
            }
        }
        auto& acc = asset_.accessors[accessor];
        acc.min = fastgltf::AccessorBoundsArray::ForType<double>(3);
        acc.max = fastgltf::AccessorBoundsArray::ForType<double>(3);
        for (std::size_t i = 0; i < 3; ++i) {
            acc.min->set<double>(i, static_cast<double>(json_number(lo[i])));
            acc.max->set<double>(i, static_cast<double>(json_number(hi[i])));
        }
    }

    std::vector<std::byte> take() { return bytes_.take(); }
    [[nodiscard]] std::size_t size() const { return bytes_.size(); }

private:
    fastgltf::Asset& asset_;
    io::ByteWriter bytes_;
};

/// fastgltf requests extras through a callback during serialization, so the
/// JSON is built up front and looked up by (category, index).
struct ExtrasTable {
    std::map<std::pair<std::uint32_t, std::size_t>, std::string> entries;

    void set(fastgltf::Category category, std::size_t index, const nlohmann::json& value) {
        entries[{static_cast<std::uint32_t>(category), index}] = value.dump();
    }

    static std::optional<std::string> callback(std::size_t index,
                                               fastgltf::Category category,
                                               void* user) {
        auto* self = static_cast<ExtrasTable*>(user);
        const auto found =
            self->entries.find({static_cast<std::uint32_t>(category), index});
        if (found == self->entries.end()) {
            return std::nullopt;
        }
        return found->second;
    }
};

class Writer {
public:
    Writer(const Model& model, const WriteOptions& options)
        : model_(model), opt_(options), buffer_(asset_) {}

    io::ParseResult<std::vector<std::byte>> run() {
        build_asset_info();
        build_materials();
        build_meshes();
        build_nodes();
        build_skins();
        build_scene();
        finish_buffer();

        fastgltf::Exporter exporter;
        if (opt_.write_extras) {
            exporter.setUserPointer(&extras_);
            exporter.setExtrasWriteCallback(&ExtrasTable::callback);
        }
        auto exported = exporter.writeGltfBinary(asset_, fastgltf::ExportOptions::None);
        if (exported.error() != fastgltf::Error::None) {
            return std::unexpected(fail(model_, ErrorKind::corrupt,
                                        std::format("fastgltf export failed: {}",
                                                    fastgltf::getErrorMessage(
                                                        exported.error()))));
        }
        return std::move(exported.get().output);
    }

private:
    const Model& model_;
    const WriteOptions& opt_;
    fastgltf::Asset asset_;
    BufferBuilder buffer_;
    ExtrasTable extras_;
    std::map<std::string, std::size_t> texture_by_path_;
    /// IR node index -> glTF node index (offset by the inserted Y-up root).
    std::vector<std::size_t> gltf_node_;

    void build_asset_info() {
        fastgltf::AssetInfo info;
        info.gltfVersion = "2.0";
        info.generator = "bethconv";
        asset_.assetInfo = std::move(info);
    }

    /// Provenance: source file, flavor, reader warnings.
    ///
    /// Put on the root node, not `asset`: fastgltf 0.9.0 accepts
    /// Category::Asset extras but never writes them.
    [[nodiscard]] nlohmann::json provenance_json() const {
        nlohmann::json j;
        j["bethconv"] = {
            {"source", json_text(model_.source)},
            {"nif_version", json_text(model_.nif_version)},
            {"nif_stream_version", model_.nif_stream},
            {"unit_scale", opt_.unit_scale},
            {"y_up", opt_.convert_to_y_up},
        };
        if (!model_.warnings.empty()) {
            nlohmann::json warnings = nlohmann::json::array();
            for (const std::string& warning : model_.warnings) {
                warnings.push_back(json_text(warning));
            }
            j["bethconv"]["warnings"] = std::move(warnings);
        }
        if (!model_.animations.empty()) {
            nlohmann::json clips = nlohmann::json::array();
            for (const AnimationClip& clip : model_.animations) {
                clips.push_back(clip_json(clip));
            }
            j["bethconv"]["animations"] = std::move(clips);
        }
        if (!model_.particles.empty()) {
            nlohmann::json systems = nlohmann::json::array();
            for (const ParticleSystem& ps : model_.particles) {
                systems.push_back(particles_json(ps));
            }
            j["bethconv"]["particles"] = std::move(systems);
        }
        return j;
    }

    static nlohmann::json floats(const std::vector<float>& values) {
        nlohmann::json out = nlohmann::json::array();
        for (const float v : values) {
            out.push_back(json_number(v));
        }
        return out;
    }

    static nlohmann::json vec(const Vec3& v) {
        return {json_number(v.x), json_number(v.y), json_number(v.z)};
    }
    static nlohmann::json vec(const Vec4& v) {
        return {json_number(v.x), json_number(v.y), json_number(v.z), json_number(v.w)};
    }

    /// Channels name nodes by IR index, which the node's own extras repeat as
    /// `id` (engines rename nodes on import). Cubic tangents are per segment,
    /// as Gamebryo stores them: v(s) = h00 v0 + h10 out0 + h01 v1 + h11 in1
    /// for s in [0, 1] between two keys.
    [[nodiscard]] static nlohmann::json clip_json(const AnimationClip& clip) {
        nlohmann::json j;
        j["name"] = json_text(clip.name);
        j["autoplay"] = clip.autoplay;
        j["cycle"] = clip.cycle == CycleMode::clamp     ? "clamp"
                     : clip.cycle == CycleMode::reverse ? "reverse"
                                                        : "loop";
        j["frequency"] = json_number(clip.frequency);
        j["phase"] = json_number(clip.phase);
        j["start"] = json_number(clip.start);
        j["stop"] = json_number(clip.stop);
        if (!clip.text_keys.empty()) {
            nlohmann::json keys = nlohmann::json::array();
            for (const auto& [time, text] : clip.text_keys) {
                keys.push_back({json_number(time), json_text(text)});
            }
            j["text_keys"] = std::move(keys);
        }
        nlohmann::json channels = nlohmann::json::array();
        for (const AnimationChannel& ch : clip.channels) {
            nlohmann::json c;
            c["node"] = ch.node;
            c["property"] = ch.property;
            c["interp"] = ch.interp == KeyInterp::step     ? "step"
                          : ch.interp == KeyInterp::cubic ? "cubic"
                                                          : "linear";
            c["components"] = ch.components;
            c["times"] = floats(ch.times);
            c["values"] = floats(ch.values);
            if (ch.interp == KeyInterp::cubic) {
                c["in"] = floats(ch.in_tangents);
                c["out"] = floats(ch.out_tangents);
            }
            channels.push_back(std::move(c));
        }
        j["channels"] = std::move(channels);
        return j;
    }

    /// The material rides along: a glTF material no mesh uses is dropped by
    /// importers.
    [[nodiscard]] nlohmann::json particles_json(const ParticleSystem& ps) const {
        nlohmann::json j;
        j["node"] = ps.node;
        if (ps.material < model_.materials.size()) {
            j["material"] = material_extras(model_.materials[ps.material])["bethconv"];
        }
        j["world_space"] = ps.world_space;
        j["max_particles"] = ps.max_particles;
        j["strip"] = ps.strip;

        nlohmann::json emitters = nlohmann::json::array();
        for (const ParticleEmitter& em : ps.emitters) {
            nlohmann::json e;
            e["kind"] = em.kind == EmitterKind::sphere     ? "sphere"
                        : em.kind == EmitterKind::cylinder ? "cylinder"
                        : em.kind == EmitterKind::mesh     ? "mesh"
                                                           : "box";
            if (em.node.has_value()) {
                e["node"] = *em.node;
            }
            e["size"] = vec(em.size);
            if (em.kind == EmitterKind::mesh) {
                e["meshes"] = em.meshes;
                e["mesh_velocity"] = em.mesh_velocity;
                e["mesh_axis"] = vec(em.mesh_axis);
            }
            e["speed"] = json_number(em.speed);
            e["speed_variation"] = json_number(em.speed_variation);
            e["declination"] = json_number(em.declination);
            e["declination_variation"] = json_number(em.declination_variation);
            e["planar_angle"] = json_number(em.planar_angle);
            e["planar_angle_variation"] = json_number(em.planar_angle_variation);
            e["color"] = vec(em.color);
            e["radius"] = json_number(em.radius);
            e["radius_variation"] = json_number(em.radius_variation);
            e["life_span"] = json_number(em.life_span);
            e["life_span_variation"] = json_number(em.life_span_variation);
            e["birth_rate"] = json_number(em.birth_rate);
            emitters.push_back(std::move(e));
        }
        j["emitters"] = std::move(emitters);

        nlohmann::json gravity = nlohmann::json::array();
        for (const ParticleGravity& g : ps.gravity) {
            nlohmann::json e;
            if (g.node.has_value()) {
                e["node"] = *g.node;
            }
            e["axis"] = vec(g.axis);
            e["strength"] = json_number(g.strength);
            e["decay"] = json_number(g.decay);
            e["spherical"] = g.spherical;
            e["turbulence"] = json_number(g.turbulence);
            e["turbulence_scale"] = json_number(g.turbulence_scale);
            e["world_aligned"] = g.world_aligned;
            gravity.push_back(std::move(e));
        }
        j["gravity"] = std::move(gravity);

        if (ps.has_rotation) {
            j["rotation"] = {
                {"speed", json_number(ps.rotation_speed)},
                {"speed_variation", json_number(ps.rotation_speed_variation)},
                {"angle", json_number(ps.rotation_angle)},
                {"angle_variation", json_number(ps.rotation_angle_variation)},
                {"random_sign", ps.rotation_random_sign},
            };
        }
        if (!ps.scales.empty()) {
            j["scales"] = floats(ps.scales);
        }
        j["grow_time"] = json_number(ps.grow_time);
        j["fade_time"] = json_number(ps.fade_time);
        j["base_scale"] = json_number(ps.base_scale);
        if (ps.has_simple_color) {
            j["simple_color"] = {
                {"fade_in", json_number(ps.fade_in)},
                {"fade_out", json_number(ps.fade_out)},
                {"color1_end", json_number(ps.color1_end)},
                {"color2_start", json_number(ps.color2_start)},
                {"color2_end", json_number(ps.color2_end)},
                {"color3_start", json_number(ps.color3_start)},
                {"colors", {vec(ps.colors[0]), vec(ps.colors[1]), vec(ps.colors[2])}},
            };
        }
        if (!ps.color_keys.empty()) {
            nlohmann::json keys = nlohmann::json::array();
            for (const auto& [time, color] : ps.color_keys) {
                keys.push_back({json_number(time), json_number(color.x), json_number(color.y),
                                json_number(color.z), json_number(color.w)});
            }
            j["color_keys"] = std::move(keys);
        }
        j["drag"] = json_number(ps.drag);
        j["drag_range"] = json_number(ps.drag_range);
        nlohmann::json drags = nlohmann::json::array();
        for (const ParticleDrag& d : ps.drags) {
            nlohmann::json e;
            if (d.node.has_value()) {
                e["node"] = *d.node;
            }
            e["axis"] = vec(d.axis);
            e["percentage"] = json_number(d.percentage);
            e["range"] = json_number(d.range);
            e["range_falloff"] = json_number(d.range_falloff);
            drags.push_back(std::move(e));
        }
        j["drags"] = std::move(drags);
        if (ps.has_subtex) {
            j["subtex"] = {
                {"start", json_number(ps.subtex_start)},
                {"start_variation", json_number(ps.subtex_start_variation)},
                {"end", json_number(ps.subtex_end)},
                {"loop_start", json_number(ps.subtex_loop_start)},
                {"loop_start_variation", json_number(ps.subtex_loop_start_variation)},
                {"frame_count", json_number(ps.subtex_frame_count)},
                {"frame_count_variation", json_number(ps.subtex_frame_count_variation)},
            };
        }
        if (!ps.subtex_offsets.empty()) {
            nlohmann::json rects = nlohmann::json::array();
            for (const Vec4& r : ps.subtex_offsets) {
                rects.push_back(vec(r));
            }
            j["subtex_offsets"] = std::move(rects);
        }
        if (!ps.unsupported.empty()) {
            nlohmann::json names = nlohmann::json::array();
            for (const std::string& name : ps.unsupported) {
                names.push_back(json_text(name));
            }
            j["unsupported"] = std::move(names);
        }
        return j;
    }

    /// Collision for the node that owns the bhkCollisionObject, not one
    /// model-level block, so multi-body NIFs (ragdolls: one per limb) keep
    /// their per-node transforms.
    [[nodiscard]] nlohmann::json collision_json_for(std::string_view node_name) const {
        nlohmann::json shapes = nlohmann::json::array();
        for (const CollisionShape& shape : model_.collision) {
            if (shape.target_node != node_name) {
                continue;
            }
            nlohmann::json j;
            j["kind"] = collision_kind_name(shape.kind);
            j["block"] = json_text(shape.block_name);
            j["node"] = json_text(shape.target_node);
            j["havok_material"] = shape.havok_material;
            j["layer"] = shape.layer;
            j["motion_type"] = shape.motion_type;
            j["quality_type"] = shape.quality_type;
            j["mass"] = json_number(shape.mass);
            j["friction"] = json_number(shape.friction);
            j["restitution"] = json_number(shape.restitution);
            j["transform"] = {
                {"translation", {shape.transform.translation.x, shape.transform.translation.y,
                                 shape.transform.translation.z}},
                {"rotation", {shape.transform.rotation.x, shape.transform.rotation.y,
                              shape.transform.rotation.z, shape.transform.rotation.w}},
            };
            switch (shape.kind) {
            case CollisionKind::box:
                j["half_extents"] = {shape.half_extents.x, shape.half_extents.y,
                                     shape.half_extents.z};
                j["radius"] = json_number(shape.radius);
                break;
            case CollisionKind::sphere:
                j["radius"] = shape.radius;
                break;
            case CollisionKind::capsule:
            case CollisionKind::cylinder:
                j["radius"] = shape.radius;
                j["point_a"] = {shape.point_a.x, shape.point_a.y, shape.point_a.z};
                j["point_b"] = {shape.point_b.x, shape.point_b.y, shape.point_b.z};
                break;
            case CollisionKind::convex_vertices:
            case CollisionKind::compressed_mesh:
            case CollisionKind::mesh: {
                if (shape.kind == CollisionKind::convex_vertices) {
                    j["radius"] = json_number(shape.radius);
                }
                // Flat vertex array: physics data, and a third the size of
                // nested triples.
                nlohmann::json verts = nlohmann::json::array();
                for (const Vec3& v : shape.vertices) {
                    verts.push_back(v.x);
                    verts.push_back(v.y);
                    verts.push_back(v.z);
                }
                j["vertices"] = std::move(verts);
                if (!shape.indices.empty()) {
                    j["indices"] = shape.indices;
                }
                break;
            }
            default:
                break;
            }
            shapes.push_back(std::move(j));
        }
        return shapes;
    }

    std::size_t texture_for(std::string_view path) {
        const std::string uri = to_uri(path);
        const auto found = texture_by_path_.find(uri);
        if (found != texture_by_path_.end()) {
            return found->second;
        }

        // `uri` stays pack-relative and unescaped wherever it names the file
        // (extras, image/texture names, dedupe key); only the href is escaped.
        const std::string reference = ascent_to_root(model_.source) + uri_escape(uri);
        const std::string href = shield_percents(reference);

        fastgltf::Image image;
        fastgltf::sources::URI source;
        source.uri = fastgltf::URI(std::string_view(href));
        source.fileByteOffset = 0;
        image.data = std::move(source);
        image.name = json_text(uri);
        asset_.images.push_back(std::move(image));

        fastgltf::Texture texture;
        texture.imageIndex = asset_.images.size() - 1;
        texture.name = json_text(uri);
        asset_.textures.push_back(std::move(texture));

        const std::size_t index = asset_.textures.size() - 1;
        texture_by_path_.emplace(uri, index);
        return index;
    }

    void build_materials() {
        for (const Material& src : model_.materials) {
            fastgltf::Material mat;
            mat.name = json_text(src.name);
            mat.pbrData.baseColorFactor = fastgltf::math::nvec4(
                json_number(src.base_color.x), json_number(src.base_color.y),
                json_number(src.base_color.z), json_number(src.base_color.w));

            // Skyrim uses specular strength and a glossiness exponent, not
            // metal/roughness. Everything is dielectric; glossiness maps to
            // roughness via the usual Blinn-Phong/GGX relation. Approximate;
            // the originals stay in extras.
            mat.pbrData.metallicFactor = 0.0f;
            mat.pbrData.roughnessFactor = roughness_from_glossiness(json_number(src.glossiness));

            mat.alphaMode = to_gltf_alpha(src.alpha_mode);
            mat.alphaCutoff = json_number(src.alpha_cutoff);
            mat.doubleSided = src.double_sided;

            // Refraction surfaces only distort what is behind them; glTF has no
            // equivalent, so they are fully transparent here and flagged in
            // extras for an engine shader.
            if (src.refraction) {
                mat.pbrData.baseColorFactor[3] = 0.0f;
                mat.alphaMode = fastgltf::AlphaMode::Blend;
            }

            const float emissive_peak =
                std::max({src.emissive.x, src.emissive.y, src.emissive.z});
            if (emissive_peak > 0.0f) {
                mat.emissiveFactor =
                    fastgltf::math::nvec3(json_number(src.emissive.x), json_number(src.emissive.y),
                                          json_number(src.emissive.z));
                if (src.emissive_strength > 1.0f) {
                    mat.emissiveStrength = json_number(src.emissive_strength);
                }
            }

            if (opt_.texture_refs != TextureRefs::none) {
                if (!src.textures[0].empty()) {
                    fastgltf::TextureInfo info;
                    info.textureIndex = texture_for(src.textures[0]);
                    mat.pbrData.baseColorTexture = std::move(info);
                }
                // Model-space normal maps are not tangent-space; they stay in
                // extras until the engine has a shader for them.
                if (!src.textures[1].empty() && !src.model_space_normals) {
                    fastgltf::NormalTextureInfo info;
                    info.textureIndex = texture_for(src.textures[1]);
                    mat.normalTexture = std::move(info);
                }
                if (src.has_glowmap && !src.textures[2].empty()) {
                    fastgltf::TextureInfo info;
                    info.textureIndex = texture_for(src.textures[2]);
                    mat.emissiveTexture = std::move(info);
                }
            }

            asset_.materials.push_back(std::move(mat));
            if (opt_.write_extras) {
                extras_.set(fastgltf::Category::Materials, asset_.materials.size() - 1,
                            material_extras(src));
            }
        }
    }

    /// Blinn-Phong exponent to GGX roughness: alpha = sqrt(2 / (exponent + 2)).
    /// Clamped so a glossiness of 0 (which vanilla has) is not fully rough.
    static float roughness_from_glossiness(float glossiness) {
        const float exponent = std::max(glossiness, 1.0f);
        const float alpha = std::sqrt(2.0f / (exponent + 2.0f));
        return std::clamp(alpha, 0.03f, 1.0f);
    }

    [[nodiscard]] nlohmann::json material_extras(const Material& src) const {
        nlohmann::json slots = nlohmann::json::object();
        for (std::size_t i = 0; i < src.textures.size(); ++i) {
            if (src.textures[i].empty()) {
                continue;
            }
            slots[std::format("{}", i)] = {
                {"role", slot_name(i, src.kind, src.bs_shader_type)},
                {"path", json_text(to_uri(src.textures[i]))},
            };
        }

        nlohmann::json j;
        j["bethconv"] = {
            {"shader", src.kind == ShaderKind::lighting  ? "BSLightingShaderProperty"
                       : src.kind == ShaderKind::effect  ? "BSEffectShaderProperty"
                       : src.kind == ShaderKind::none    ? "none"
                                                         : "other"},
            {"shader_type", src.bs_shader_type},
            {"glossiness", src.glossiness},
            {"specular_strength", src.specular_strength},
            {"specular_color", {src.specular_color.x, src.specular_color.y,
                                src.specular_color.z}},
            {"emissive_multiple", src.emissive_strength},
            {"emissive_color", {json_number(src.emissive.x), json_number(src.emissive.y),
                                json_number(src.emissive.z)}},
            {"uv_offset", {src.uv_offset.x, src.uv_offset.y}},
            {"uv_scale", {src.uv_scale.x, src.uv_scale.y}},
            {"environment_map_scale", src.environment_map_scale},
            {"model_space_normals", src.model_space_normals},
            {"skin_tinted", src.skin_tinted},
            {"face_tinted", src.face_tinted},
            {"has_glowmap", src.has_glowmap},
            {"has_environment_map", src.has_environment_map},
            {"has_backlight", src.has_backlight},
            {"has_rimlight", src.has_rimlight},
            {"has_softlight", src.has_softlight},
            {"alpha_property", src.alpha_property_present},
            {"alpha_flags", src.alpha_flags},
            {"alpha_cutoff", json_number(src.alpha_cutoff)},
            {"shader_flags1", src.shader_flags1},
            {"shader_flags2", src.shader_flags2},
            {"refraction", src.refraction},
            {"emissive_alpha", json_number(src.emissive_alpha)},
            {"falloff", nlohmann::json::array({json_number(src.falloff.x), json_number(src.falloff.y),
                                               json_number(src.falloff.z), json_number(src.falloff.w)})},
            {"soft_falloff_depth", json_number(src.soft_falloff_depth)},
            {"texture_slots", std::move(slots)},
        };
        return j;
    }

    /// One glTF mesh per IR primitive, so shapes keep their names and can be
    /// addressed individually (a lantern's flame, a door's handle).
    void build_meshes() {
        for (const Primitive& prim : model_.primitives) {
            fastgltf::Primitive gp;
            gp.type = fastgltf::PrimitiveType::Triangles;

            const std::size_t pos = buffer_.add(prim.positions, fastgltf::AccessorType::Vec3,
                                                fastgltf::ComponentType::Float, false, 3);
            buffer_.set_position_bounds(pos, prim.positions);
            gp.attributes.emplace_back(fastgltf::Attribute{"POSITION", pos});

            if (!prim.normals.empty()) {
                gp.attributes.emplace_back(fastgltf::Attribute{
                    "NORMAL", buffer_.add(prim.normals, fastgltf::AccessorType::Vec3,
                                          fastgltf::ComponentType::Float, false, 3)});
            }
            if (!prim.tangents.empty()) {
                gp.attributes.emplace_back(fastgltf::Attribute{
                    "TANGENT", buffer_.add(prim.tangents, fastgltf::AccessorType::Vec4,
                                           fastgltf::ComponentType::Float, false, 4)});
            }
            if (!prim.uvs.empty()) {
                gp.attributes.emplace_back(fastgltf::Attribute{
                    "TEXCOORD_0", buffer_.add(prim.uvs, fastgltf::AccessorType::Vec2,
                                              fastgltf::ComponentType::Float, false, 2)});
            }
            if (!prim.colors.empty()) {
                gp.attributes.emplace_back(fastgltf::Attribute{
                    "COLOR_0", buffer_.add(prim.colors, fastgltf::AccessorType::Vec4,
                                           fastgltf::ComponentType::Float, false, 4)});
            }
            if (!prim.joints.empty()) {
                gp.attributes.emplace_back(fastgltf::Attribute{
                    "JOINTS_0",
                    buffer_.add(prim.joints, fastgltf::AccessorType::Vec4,
                                fastgltf::ComponentType::UnsignedShort, false, 4)});
                gp.attributes.emplace_back(fastgltf::Attribute{
                    "WEIGHTS_0", buffer_.add(prim.weights, fastgltf::AccessorType::Vec4,
                                             fastgltf::ComponentType::Float, false, 4)});
            }

            gp.indicesAccessor =
                buffer_.add(prim.indices, fastgltf::AccessorType::Scalar,
                            fastgltf::ComponentType::UnsignedInt, true, 1);
            gp.materialIndex = prim.material;

            fastgltf::Mesh mesh;
            mesh.name = json_text(prim.name);
            mesh.primitives.emplace_back(std::move(gp));
            asset_.meshes.push_back(std::move(mesh));
        }
    }

    void build_nodes() {
        gltf_node_.resize(model_.nodes.size());
        for (std::size_t i = 0; i < model_.nodes.size(); ++i) {
            const Node& src = model_.nodes[i];
            fastgltf::Node node;
            node.name = json_text(src.name);

            fastgltf::TRS trs;
            trs.translation = fastgltf::math::fvec3(json_number(src.transform.translation.x),
                                                    json_number(src.transform.translation.y),
                                                    json_number(src.transform.translation.z));
            trs.rotation =
                fastgltf::math::fquat(json_number(src.transform.rotation.x),
                                      json_number(src.transform.rotation.y),
                                      json_number(src.transform.rotation.z),
                                      json_number(src.transform.rotation.w));
            trs.scale = fastgltf::math::fvec3(json_number(src.transform.scale.x),
                                              json_number(src.transform.scale.y),
                                              json_number(src.transform.scale.z));
            node.transform = trs;

            asset_.nodes.push_back(std::move(node));
            gltf_node_[i] = asset_.nodes.size() - 1;
        }

        // Second pass: children and meshes, now that every index is known.
        for (std::size_t i = 0; i < model_.nodes.size(); ++i) {
            const Node& src = model_.nodes[i];
            fastgltf::Node& node = asset_.nodes[gltf_node_[i]];
            for (const std::size_t child : src.children) {
                node.children.emplace_back(gltf_node_[child]);
            }
            if (opt_.write_extras) {
                nlohmann::json ours = nlohmann::json::object();
                nlohmann::json collision = collision_json_for(src.name);
                if (!collision.empty()) {
                    ours["collision"] = std::move(collision);
                }
                if (src.billboard_mode.has_value()) {
                    ours["billboard"] = *src.billboard_mode;
                }
                if (src.referenced) {
                    ours["id"] = i;
                }
                if (src.hidden) {
                    ours["hidden"] = true;
                }
                if (!ours.empty()) {
                    extras_.set(fastgltf::Category::Nodes, gltf_node_[i],
                                nlohmann::json{{"bethconv", std::move(ours)}});
                }
            }

            if (src.primitives.size() == 1) {
                node.meshIndex = src.primitives.front();
                if (model_.primitives[src.primitives.front()].skin.has_value()) {
                    node.skinIndex = *model_.primitives[src.primitives.front()].skin;
                }
            } else {
                // Several shapes per node are allowed by the format; put the
                // extras on child nodes rather than dropping all but one.
                for (const std::size_t prim : src.primitives) {
                    fastgltf::Node extra;
                    extra.name = json_text(model_.primitives[prim].name);
                    extra.meshIndex = prim;
                    if (model_.primitives[prim].skin.has_value()) {
                        extra.skinIndex = *model_.primitives[prim].skin;
                    }
                    extra.transform = fastgltf::TRS{};
                    asset_.nodes.push_back(std::move(extra));
                    node.children.emplace_back(asset_.nodes.size() - 1);
                }
            }
        }
    }

    void build_skins() {
        for (const Skin& src : model_.skins) {
            fastgltf::Skin skin;
            skin.name = json_text(src.name);
            for (const std::size_t joint : src.joints) {
                skin.joints.emplace_back(gltf_node_[joint]);
            }

            std::vector<float> matrices;
            matrices.reserve(src.inverse_bind_matrices.size() * 16);
            for (const Mat4& m : src.inverse_bind_matrices) {
                matrices.insert(matrices.end(), m.m.begin(), m.m.end());
            }
            skin.inverseBindMatrices =
                buffer_.add(matrices, fastgltf::AccessorType::Mat4,
                            fastgltf::ComponentType::Float, false, 16);
            asset_.skins.push_back(std::move(skin));
        }
    }

    /// Axis and unit conversion as a single root node. Baking it into vertices
    /// would make buffers impossible to diff against the NIF.
    void build_scene() {
        fastgltf::Scene scene;
        scene.name = "root";

        std::vector<std::size_t> roots;
        for (const std::size_t root : model_.roots) {
            roots.push_back(gltf_node_[root]);
        }

        if (opt_.convert_to_y_up || opt_.unit_scale != 1.0f) {
            fastgltf::Node pivot;
            pivot.name = "bethconv_z_up_to_y_up";
            fastgltf::TRS trs;
            if (opt_.convert_to_y_up) {
                // -90 degrees about X: Z-up to Y-up, same handedness.
                const float half = -std::numbers::pi_v<float> / 4.0f;
                trs.rotation = fastgltf::math::fquat(std::sin(half), 0.0f, 0.0f,
                                                     std::cos(half));
            }
            trs.scale = fastgltf::math::fvec3(opt_.unit_scale, opt_.unit_scale,
                                              opt_.unit_scale);
            pivot.transform = trs;
            for (const std::size_t root : roots) {
                pivot.children.emplace_back(root);
            }
            asset_.nodes.push_back(std::move(pivot));
            scene.nodeIndices.emplace_back(asset_.nodes.size() - 1);
            if (opt_.write_extras) {
                extras_.set(fastgltf::Category::Nodes, asset_.nodes.size() - 1,
                            provenance_json());
            }
        } else {
            for (const std::size_t root : roots) {
                scene.nodeIndices.emplace_back(root);
            }
            if (opt_.write_extras && !roots.empty()) {
                extras_.set(fastgltf::Category::Nodes, roots.front(), provenance_json());
            }
        }

        asset_.scenes.push_back(std::move(scene));
        asset_.defaultScene = 0;
    }

    void finish_buffer() {
        if (buffer_.size() == 0) {
            // glTF requires byteLength >= 1, so a model without geometry
            // (skeleton, bare node tree) gets no buffer at all. Godot accepts an
            // empty one, but fastgltf and the Khronos validator do not.
            return;
        }
        fastgltf::Buffer buffer;
        buffer.byteLength = buffer_.size();
        fastgltf::sources::Vector source;
        source.bytes = buffer_.take();
        source.mimeType = fastgltf::MimeType::GltfBuffer;
        buffer.data = std::move(source);
        asset_.buffers.push_back(std::move(buffer));
    }
};

} // namespace

io::ParseResult<std::vector<std::byte>> write_glb(const Model& model,
                                                  const WriteOptions& options) {
    try {
        Writer writer(model, options);
        return writer.run();
    } catch (const std::exception& e) {
        return std::unexpected(
            fail(model, ErrorKind::corrupt, std::format("glTF writer threw: {}", e.what())));
    }
}

io::ParseResult<std::size_t> write_glb_file(const Model& model,
                                            const std::filesystem::path& path,
                                            const WriteOptions& options) {
    auto glb = write_glb(model, options);
    if (!glb.has_value()) {
        return std::unexpected(glb.error());
    }

    std::string error;
    if (!io::write_file(path, *glb, error)) {
        return std::unexpected(fail(model, ErrorKind::bad_value, std::move(error)));
    }
    return glb->size();
}

std::string escape_texture_uri(std::string_view path) { return uri_escape(path); }

} // namespace bethconv::mesh
