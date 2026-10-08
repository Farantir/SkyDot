// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/materials.hpp"

#include "render/effect_asset.hpp"
#include "render/image_space.hpp"
#include "render/lod.hpp"
#include "render/particles.hpp"
#include "render/shader_source.hpp"
#include "render/terrain.hpp"
#include "render/water.hpp"
#include "render/weather.hpp"

#include "skydot_formats/units.hpp"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/cubemap.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector4.hpp>
#include <godot_cpp/variant/packed_vector4_array.hpp>

#include <algorithm>
#include <array>
#include <mutex>
#include <vector>

using godot::Array;
using godot::Color;
using godot::Dictionary;
using godot::Ref;
using godot::String;
using godot::Variant;

namespace skydot {

namespace {

// Shader flags (nifly SLSF1_* / SLSF2_*; UESP, BSLightingShaderProperty).
constexpr std::uint32_t k_sf1_specular = 1u << 0;
constexpr std::uint32_t k_sf1_vertex_alpha = 1u << 3;
constexpr std::uint32_t k_sf1_greyscale_to_palette_color = 1u << 4;
constexpr std::uint32_t k_sf1_greyscale_to_palette_alpha = 1u << 5;
constexpr std::uint32_t k_sf1_use_falloff = 1u << 6;
constexpr std::uint32_t k_sf1_environment_mapping = 1u << 7;
constexpr std::uint32_t k_sf1_own_emit = 1u << 22;
constexpr std::uint32_t k_sf1_soft_effect = 1u << 30;
constexpr std::uint32_t k_sf2_double_sided = 1u << 4;
constexpr std::uint32_t k_sf2_vertex_colors = 1u << 5;
constexpr std::uint32_t k_sf2_glow_map = 1u << 6;
constexpr std::uint32_t k_sf2_tree_anim = 1u << 29;
constexpr std::uint32_t k_sf2_effect_lighting = 1u << 30;

// BSLightingShaderProperty shader types.
constexpr std::int64_t k_type_envmap = 1;
constexpr std::int64_t k_type_glowmap = 2;
constexpr std::int64_t k_type_face_tint = 4;
constexpr std::int64_t k_type_skin_tint = 5;
constexpr std::int64_t k_type_hair_tint = 6;

// NiAlphaProperty flags: bit 0 blending, bits 1-4 source factor, bits 5-8
// destination factor, bit 9 alpha test. Factor 0 is ONE.
constexpr std::uint32_t k_alpha_blend = 1u << 0;
constexpr std::uint32_t k_alpha_test = 1u << 9;

enum class Alpha { none, test, blend, add, mul };

/// The shader code of each variant: a stub that Godot's preprocessor expands
/// (shader_source::variant). Everything else is a uniform, so these are all
/// the shaders materials ever need (see SkydotMaterials::warm_up).
std::vector<std::string> sided(bool double_sided) {
    return double_sided ? std::vector<std::string>{"SKYDOT_DOUBLE_SIDED"} : std::vector<std::string>{};
}

std::string lighting_code(bool double_sided, Alpha alpha) {
    std::vector<std::string> defines = sided(double_sided);
    if (alpha == Alpha::test) {
        defines.emplace_back("SKYDOT_ALPHA_TEST");
    } else if (alpha == Alpha::blend) {
        defines.emplace_back("SKYDOT_ALPHA_BLEND");
    } else if (alpha == Alpha::mul) {
        defines.emplace_back("SKYDOT_ALPHA_MUL");
    }
    return shader_source::variant("lighting.gdshaderinc", defines);
}

std::string effect_code(bool double_sided, Alpha alpha, bool particles = false, bool lit = false) {
    std::vector<std::string> defines = sided(double_sided);
    if (particles) {
        defines.emplace_back("SKYDOT_PARTICLES");
    }
    if (lit) {
        defines.emplace_back("SKYDOT_LIT");
    }
    if (alpha == Alpha::add) {
        defines.emplace_back("SKYDOT_ALPHA_ADD");
    } else if (alpha == Alpha::mul) {
        defines.emplace_back("SKYDOT_ALPHA_MUL");
    } else if (alpha == Alpha::blend) {
        defines.emplace_back("SKYDOT_ALPHA_BLEND");
    } else if (alpha == Alpha::test) {
        defines.emplace_back("SKYDOT_ALPHA_TEST");
    }
    return shader_source::variant("effect.gdshaderinc", defines);
}

std::string refraction_code(bool double_sided) {
    return shader_source::variant("refraction.gdshaderinc", sided(double_sided));
}

std::uint32_t as_u32(const Dictionary& d, const char* key) {
    const Variant v = d.get(key, 0);
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(static_cast<double>(v)));
}

/// NiAlphaProperty blending the framebuffer by the shape's colour: source
/// ZERO and destination SRC_COLOR, or DEST_COLOR and ZERO (factors 1, 2, 4).
bool multiplies(std::uint32_t alpha_flags) {
    const std::uint32_t source = (alpha_flags >> 1) & 0xFu;
    const std::uint32_t destination = (alpha_flags >> 5) & 0xFu;
    return (alpha_flags & k_alpha_blend) != 0 &&
           ((source == 1 && destination == 2) || (source == 4 && destination == 1));
}

double as_double(const Dictionary& d, const char* key, double fallback) {
    const Variant v = d.get(key, fallback);
    return v.get_type() == Variant::FLOAT || v.get_type() == Variant::INT ? static_cast<double>(v)
                                                                            : fallback;
}

godot::Vector2 as_vec2(const Dictionary& d, const char* key, godot::Vector2 fallback) {
    const Variant v = d.get(key, Variant());
    if (v.get_type() != Variant::ARRAY) {
        return fallback;
    }
    const Array a = v;
    return a.size() >= 2 ? godot::Vector2(static_cast<float>(static_cast<double>(a[0])),
                                          static_cast<float>(static_cast<double>(a[1])))
                         : fallback;
}

godot::Vector3 as_vec3(const Dictionary& d, const char* key, godot::Vector3 fallback) {
    const Variant v = d.get(key, Variant());
    if (v.get_type() != Variant::ARRAY) {
        return fallback;
    }
    const Array a = v;
    return a.size() >= 3 ? godot::Vector3(static_cast<float>(static_cast<double>(a[0])),
                                          static_cast<float>(static_cast<double>(a[1])),
                                          static_cast<float>(static_cast<double>(a[2])))
                         : fallback;
}

/// The path of texture slot `slot` from the extras, or "".
String slot_path(const Dictionary& extras, int slot) {
    const Variant slots = extras.get("texture_slots", Variant());
    if (slots.get_type() != Variant::DICTIONARY) {
        return String();
    }
    const Variant entry = Dictionary(slots).get(String::num_int64(slot), Variant());
    if (entry.get_type() != Variant::DICTIONARY) {
        return String();
    }
    return Dictionary(entry).get("path", String());
}

void collect_meshes(godot::Node* node, godot::TypedArray<godot::Node>& out) {
    if (godot::Object::cast_to<godot::MeshInstance3D>(node) != nullptr) {
        out.push_back(node);
    }
    for (int i = 0; i < node->get_child_count(); ++i) {
        collect_meshes(node->get_child(i), out);
    }
}

} // namespace

void SkydotMaterials::_bind_methods() {
    using godot::D_METHOD;
    godot::ClassDB::bind_method(D_METHOD("apply", "root"), &SkydotMaterials::apply);
    godot::ClassDB::bind_method(D_METHOD("warm_up"), &SkydotMaterials::warm_up);
    godot::ClassDB::bind_static_method(get_class_static(), D_METHOD("shader_sources"),
                                       &SkydotMaterials::shader_sources);
    godot::ClassDB::bind_static_method(get_class_static(), D_METHOD("sync_fog", "environment"),
                                       &SkydotMaterials::sync_fog);
    godot::ClassDB::bind_static_method(get_class_static(), D_METHOD("set_game_light", "light", "gamma"),
                                       &SkydotMaterials::set_game_light);
    godot::ClassDB::bind_static_method(get_class_static(), D_METHOD("game_color", "gamma"),
                                       &SkydotMaterials::game_color);
    godot::ClassDB::bind_method(D_METHOD("convert", "source"), &SkydotMaterials::convert);
    godot::ClassDB::bind_method(D_METHOD("get_material_count"),
                                &SkydotMaterials::get_material_count);
    godot::ClassDB::bind_method(D_METHOD("get_shader_count"), &SkydotMaterials::get_shader_count);
}

std::int64_t SkydotMaterials::get_material_count() const {
    return static_cast<std::int64_t>(materials_.size());
}

std::int64_t SkydotMaterials::get_shader_count() const {
    return static_cast<std::int64_t>(shaders_.size());
}

void SkydotMaterials::ensure_fog_globals() {
    // Once per process (materials are made on worker threads too); listing
    // the existing parameters is editor-only.
    static std::once_flag once;
    std::call_once(once, [] {
        // x begin, y end (metres), z curve, w density: Environment's depth fog.
        auto* rs = godot::RenderingServer::get_singleton();
        rs->global_shader_parameter_add("skydot_fog", godot::RenderingServer::GLOBAL_VAR_TYPE_VEC4,
                                        godot::Vector4(0, 1, 1, 0));
        rs->global_shader_parameter_add("skydot_fog_near_color", godot::RenderingServer::GLOBAL_VAR_TYPE_VEC3,
                                        godot::Vector3());
        rs->global_shader_parameter_add("skydot_fog_far_color", godot::RenderingServer::GLOBAL_VAR_TYPE_VEC3,
                                        godot::Vector3());
        for (const char* name : {"skydot_sun_direction", "skydot_sun_color", "skydot_sky_upper", "skydot_sky_horizon", "skydot_effect_light"}) {
            rs->global_shader_parameter_add(name, godot::RenderingServer::GLOBAL_VAR_TYPE_VEC3, godot::Vector3());
        }
        rs->global_shader_parameter_add("skydot_foliage_bias", godot::RenderingServer::GLOBAL_VAR_TYPE_FLOAT,
                                        0.5F);
        for (const char* name : {"skydot_ambient_r", "skydot_ambient_g", "skydot_ambient_b"}) {
            rs->global_shader_parameter_add(name, godot::RenderingServer::GLOBAL_VAR_TYPE_VEC4,
                                            godot::Vector4(0, 0, 0, 0.3F));
        }
    });
}

void SkydotMaterials::set_foliage_bias(float bias) {
    ensure_fog_globals();
    godot::RenderingServer::get_singleton()->global_shader_parameter_set("skydot_foliage_bias", bias);
}

void SkydotMaterials::sync_fog(const Ref<godot::Environment>& environment) {
    ensure_fog_globals();
    godot::Vector4 fog(0, 1, 1, 0);
    if (environment.is_valid() && environment->is_fog_enabled() &&
        environment->get_fog_mode() == godot::Environment::FOG_MODE_DEPTH) {
        fog = godot::Vector4(environment->get_fog_depth_begin(), environment->get_fog_depth_end(),
                             environment->get_fog_depth_curve(), environment->get_fog_density());
    }
    const Color far = environment.is_valid() ? environment->get_fog_light_color() : Color();
    const Color near = environment.is_valid()
                           ? static_cast<Color>(environment->get_meta("skydot_fog_near_color", far))
                           : far;
    const auto vec = [](const Color& c) { return godot::Vector3(c.r, c.g, c.b); };
    auto* rs = godot::RenderingServer::get_singleton();
    rs->global_shader_parameter_set("skydot_fog", fog);
    rs->global_shader_parameter_set("skydot_fog_near_color", vec(near));
    rs->global_shader_parameter_set("skydot_fog_far_color", vec(far));
    // The sun and sky as water reflects them, and the light lit effects
    // take (SkydotWeather sets these metas; inside there is no sun).
    for (const char* name : {"skydot_sun_direction", "skydot_sun_color", "skydot_sky_upper", "skydot_sky_horizon", "skydot_effect_light"}) {
        const godot::StringName key = name;
        const Variant v = environment.is_valid() && environment->has_meta(key) ? environment->get_meta(key) : Variant();
        rs->global_shader_parameter_set(key, v.get_type() == Variant::VECTOR3 ? static_cast<godot::Vector3>(v)
                                                                             : godot::Vector3());
    }

    // Six colours, one per side; a surface facing +z takes the z+ colour.
    // Per channel the shaders get a linear function of the normal: half of
    // plus side minus minus side per axis and the mean of all six. The
    // game's State::directionalAmbientTransform (SkyrimRemote `@ambient`)
    // reads the other way round, but its frames do not: with that sign the
    // Sleeping Giant Inn's floors take the bright z- 0.27 and come out
    // twice as bright as the game's (ref21 floor 43 vs 23, with this sign
    // 23), and all five interior and four of five exterior comparison shots
    // move away from the game (grid error 29.4 -> 46.1 and 75.3 -> 85.2).
    std::array<Color, 6> sides;
    const Variant meta = environment.is_valid() && environment->has_meta("skydot_directional_ambient")
                             ? environment->get_meta("skydot_directional_ambient")
                             : Variant();
    if (meta.get_type() == Variant::ARRAY && static_cast<Array>(meta).size() >= 6) {
        const Array a = meta;
        for (int i = 0; i < 6; ++i) {
            sides[static_cast<std::size_t>(i)] = a[i];
        }
    } else {
        sides.fill(environment.is_valid() ? environment->get_ambient_light_color() : Color(0.3F, 0.3F, 0.3F));
    }
    for (int channel = 0; channel < 3; ++channel) {
        const auto side = [&](int i) { return sides[static_cast<std::size_t>(i)][channel]; };
        const godot::Vector3 game((side(0) - side(1)) * 0.5F, (side(2) - side(3)) * 0.5F,
                                  (side(4) - side(5)) * 0.5F);
        const godot::Vector3 world(game.x, game.z, -game.y); // the game's z up to Godot's y up
        const float mean = (side(0) + side(1) + side(2) + side(3) + side(4) + side(5)) / 6.0F;
        static const char* const names[] = {"skydot_ambient_r", "skydot_ambient_g", "skydot_ambient_b"};
        rs->global_shader_parameter_set(names[channel], godot::Vector4(world.x, world.y, world.z, mean));
    }
}

Color SkydotMaterials::game_color(const Color& gamma) {
    return Color(std::clamp(gamma.r, 0.0F, 1.0F), std::clamp(gamma.g, 0.0F, 1.0F),
                 std::clamp(gamma.b, 0.0F, 1.0F), gamma.a)
        .linear_to_srgb();
}

void SkydotMaterials::set_game_light(godot::Light3D* light, const Color& gamma) {
    if (light == nullptr) {
        return;
    }
    const float peak = std::max({gamma.r, gamma.g, gamma.b, 1.0F});
    light->set_color(game_color(Color(gamma.r / peak, gamma.g / peak, gamma.b / peak)));
    light->set_param(godot::Light3D::PARAM_ENERGY, peak);
}

Ref<godot::Shader> SkydotMaterials::shader_for(const std::string& code) {
    ensure_fog_globals();
    auto it = shaders_.find(code);
    if (it != shaders_.end()) {
        return it->second;
    }
    Ref<godot::Shader> shader;
    shader.instantiate();
    shader->set_code(String::utf8(code.c_str(), static_cast<int>(code.size())));
    shaders_.emplace(code, shader);
    return shader;
}

Ref<godot::Texture> SkydotMaterials::load_texture(const String& vpath) {
    if (vpath.is_empty()) {
        return {};
    }
    const auto key_utf8 = vpath.utf8();
    const std::string key(key_utf8.get_data(), static_cast<std::size_t>(key_utf8.length()));
    if (auto it = textures_.find(key); it != textures_.end()) {
        return it->second;
    }
    Ref<godot::Texture> texture =
        assets_ != nullptr ? assets_->texture(key) : Ref<godot::Texture>();
    textures_.emplace(key, texture);
    return texture;
}

Ref<godot::Material> SkydotMaterials::convert(const Ref<godot::Material>& source) {
    if (source.is_null()) {
        return {};
    }
    const std::uint64_t id = source->get_instance_id();
    if (auto it = materials_.find(id); it != materials_.end()) {
        return it->second;
    }

    Ref<godot::Material> result;
    const Ref<godot::BaseMaterial3D> base = source;
    const Variant extras_meta =
        source->has_meta("extras") ? source->get_meta("extras") : Variant();
    if (base.is_null() || extras_meta.get_type() != Variant::DICTIONARY) {
        materials_.emplace(id, result);
        return result;
    }
    const Variant block = Dictionary(extras_meta).get("bethconv", Variant());
    if (block.get_type() != Variant::DICTIONARY) {
        materials_.emplace(id, result);
        return result;
    }
    const Dictionary extras = block;

    const String kind = extras.get("shader", String());
    const std::uint32_t flags1 = as_u32(extras, "shader_flags1");
    const std::uint32_t flags2 = as_u32(extras, "shader_flags2");
    const bool refraction = static_cast<bool>(extras.get("refraction", false));
    const bool double_sided = base->get_cull_mode() == godot::BaseMaterial3D::CULL_DISABLED ||
                              (flags2 & k_sf2_double_sided) != 0;
    const godot::Vector2 uv_scale = as_vec2(extras, "uv_scale", godot::Vector2(1, 1));
    const godot::Vector2 uv_offset = as_vec2(extras, "uv_offset", godot::Vector2(0, 0));

    // Pack meshes carry no glTF images: the extras name every texture
    // (formats/pack-format.md, "Texture references in meshes").
    const Ref<godot::Texture> albedo = load_texture(slot_path(extras, 0));
    const bool model_space = static_cast<bool>(extras.get("model_space_normals", false));
    const Ref<godot::Texture> normal_map = load_texture(slot_path(extras, 1));

    Ref<godot::ShaderMaterial> out;
    out.instantiate();
    out->set_name(source->get_name());

    if (refraction) {
        out->set_shader(shader_for(refraction_code(double_sided)));
        const Ref<godot::Texture> normal = normal_map.is_valid() ? normal_map : albedo;
        out->set_shader_parameter("normal_tex", normal);
        // The screen texture holds only opaque geometry, so a refraction
        // surface drawn after flames or glows would paint over them. Draw it
        // first among transparent surfaces instead.
        out->set_render_priority(godot::Material::RENDER_PRIORITY_MIN);
    } else if (kind == "BSWaterShaderProperty") {
        // Placed water (streams, ponds): SkydotWorld gives it its water
        // type's material (tagged here); elsewhere it stays translucent blue.
        out->set_shader(shader_for(lighting_code(false, Alpha::blend)));
        out->set_shader_parameter("base_color", shader_rgba(Color(0.25F, 0.35F, 0.4F, 0.6F)));
        out->set_meta("skydot_water", true);
    } else if (kind == "BSEffectShaderProperty") {
        // The importer gamma-encodes glTF's emissiveFactor; undo that to get
        // the NIF's value back.
        configure_effect(out, extras, double_sided, false, albedo,
                         base->get_emission().srgb_to_linear());
    } else {
        const auto transparency = base->get_transparency();
        Alpha alpha = Alpha::none;
        if (transparency == godot::BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR) {
            alpha = Alpha::test;
        } else if (transparency == godot::BaseMaterial3D::TRANSPARENCY_ALPHA ||
                   transparency == godot::BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS) {
            alpha = Alpha::blend;
        }
        // glTF has one mode; a NIF may blend and test at once (FaceGen's
        // shaved-hair layer: test at 0, so the importer kept an opaque
        // mask). The game blends, discarding below the threshold.
        const std::uint32_t alpha_flags = as_u32(extras, "alpha_flags");
        const bool blend_and_test = static_cast<bool>(extras.get("alpha_property", false)) &&
                                    (alpha_flags & k_alpha_blend) != 0 && (alpha_flags & k_alpha_test) != 0;
        if (blend_and_test) {
            alpha = Alpha::blend;
        }
        if (static_cast<bool>(extras.get("alpha_property", false)) && multiplies(alpha_flags)) {
            alpha = Alpha::mul; // gems, stained glass
        }
        const std::int64_t shader_type = static_cast<std::int64_t>(as_double(extras, "shader_type", 0));
        Ref<godot::Texture> glow;
        if (shader_type == k_type_glowmap || (flags2 & k_sf2_glow_map) != 0) {
            glow = load_texture(slot_path(extras, 2));
        }
        Ref<godot::Texture> env;
        Ref<godot::Texture> env_mask;
        if (shader_type == k_type_envmap && (flags1 & k_sf1_environment_mapping) != 0) {
            env = load_texture(slot_path(extras, 4));
            env_mask = load_texture(slot_path(extras, 5));
        }
        const bool env_map = godot::Object::cast_to<godot::Cubemap>(env.ptr()) != nullptr;

        out->set_shader(shader_for(lighting_code(double_sided, alpha)));
        out->set_shader_parameter("use_vertex_colors",
                                  (flags2 & k_sf2_vertex_colors) != 0 && shader_type != k_type_hair_tint);
        // On trees, vertex alpha is the wind weight, not opacity.
        out->set_shader_parameter("use_vertex_alpha", (flags1 & k_sf1_vertex_alpha) != 0 &&
                                                          (flags2 & k_sf2_tree_anim) == 0);
        out->set_shader_parameter("use_glow_map", glow.is_valid());
        // Thin cut-out foliage: trees (their shader flag) share the bias; grass
        // gets it where its materials are copied (cell_builder.cpp).
        out->set_shader_parameter("foliage", alpha == Alpha::test && (flags2 & k_sf2_tree_anim) != 0);
        // A STAT's directional material lands on its shapes whether or not
        // they have the Projected UV flag: Nordic towers share one model
        // between the snowy and the bare STAT, without the flag, and only
        // the snowy one shows snow in the game (comparison shot ref14).
        // Skin and FaceGen are left alone.
        if (!model_space && shader_type != k_type_skin_tint && shader_type != k_type_face_tint) {
            out->set_meta("skydot_projected_uv", true); // see projected()
        }
        out->set_shader_parameter("own_emit", glow.is_null() && (flags1 & k_sf1_own_emit) != 0);
        out->set_shader_parameter("albedo_tex", albedo);
        out->set_shader_parameter("normal_tex", normal_map);
        out->set_shader_parameter("model_space_normals", model_space && normal_map.is_valid());
        // Slot 7 is the specular map on skin (type 5) and FaceGen heads (4);
        // elsewhere it is a backlight map.
        if (shader_type == k_type_skin_tint || shader_type == k_type_face_tint) {
            if (const Ref<godot::Texture> spec = load_texture(slot_path(extras, 7)); spec.is_valid()) {
                out->set_shader_parameter("use_spec_tex", true);
                out->set_shader_parameter("spec_tex", spec);
            }
        }
        out->set_shader_parameter("use_skin_tint", shader_type == k_type_skin_tint);
        if (shader_type == k_type_skin_tint) {
            out->set_shader_parameter("skin_tint", as_vec3(extras, "skin_tint_color", godot::Vector3(0.5, 0.5, 0.5)));
        }
        if (shader_type == k_type_hair_tint) {
            // Packs before mesh/16 have no colour: untinted.
            out->set_shader_parameter("use_hair_tint", true);
            out->set_shader_parameter("hair_tint", as_vec3(extras, "hair_tint_color", godot::Vector3(1, 1, 1)));
        }
        if (shader_type == k_type_face_tint) {
            if (const Ref<godot::Texture> tint = load_texture(slot_path(extras, 6)); tint.is_valid()) {
                out->set_shader_parameter("use_face_tint", true);
                out->set_shader_parameter("face_tint_tex", tint);
            }
        }
        // The importer gamma-encodes glTF's factors; the shaders want the
        // NIF's numbers (see materials.hpp).
        out->set_shader_parameter("base_color", shader_rgba(base->get_albedo().srgb_to_linear()));
        out->set_shader_parameter("specular_color",
                                  as_vec3(extras, "specular_color", godot::Vector3(1, 1, 1)));
        // Without the Specular flag the game draws no highlight at all.
        out->set_shader_parameter("specular_strength", (flags1 & k_sf1_specular) != 0
                                                           ? as_double(extras, "specular_strength", 1.0)
                                                           : 0.0);
        out->set_shader_parameter("glossiness", as_double(extras, "glossiness", 30.0));
        out->set_shader_parameter("emission_color", shader_rgb(base->get_emission().srgb_to_linear()));
        out->set_shader_parameter("emission_strength", as_double(extras, "emissive_multiple", 1.0));
        out->set_shader_parameter("soft_depth", as_double(extras, "soft_falloff_depth", 10.0) *
                                                    formats::k_metres_per_unit);
        out->set_shader_parameter("alpha_cutoff", base->get_alpha_scissor_threshold());
        if (blend_and_test) {
            out->set_shader_parameter("blend_test", true);
            out->set_shader_parameter("alpha_cutoff", as_double(extras, "alpha_cutoff", 0.0));
        }
        if (glow.is_valid()) {
            out->set_shader_parameter("glow_tex", glow);
        }
        if (env_map) {
            out->set_shader_parameter("use_env_map", true);
            out->set_shader_parameter("env_tex", env);
            if (env_mask.is_valid()) {
                out->set_shader_parameter("use_env_mask", true);
                out->set_shader_parameter("env_mask_tex", env_mask);
            }
            out->set_shader_parameter("env_scale", as_double(extras, "environment_map_scale", 1.0));
        }
    }
    out->set_shader_parameter("uv_scale", uv_scale);
    out->set_shader_parameter("uv_offset", uv_offset);
    if (kind == "BSEffectShaderProperty" && !refraction) {
        out->set_shader_parameter("alpha_cutoff", base->get_alpha_scissor_threshold());
    }

    result = out;
    materials_.emplace(id, result);
    return result;
}

Ref<godot::ShaderMaterial> SkydotMaterials::projected(const Ref<godot::ShaderMaterial>& material,
                                                      std::uint32_t key, const ProjectedMaterial& with) {
    if (material.is_null() || !material->has_meta("skydot_projected_uv")) {
        return material;
    }
    const std::pair<std::uint64_t, std::uint32_t> id{material->get_instance_id(), key};
    const std::scoped_lock lock(projected_mutex_);
    if (auto it = projected_.find(id); it != projected_.end()) {
        return it->second;
    }
    Ref<godot::ShaderMaterial> out = material->duplicate();
    out->set_shader_parameter("use_projection", true);
    out->set_shader_parameter("proj_textured", with.albedo.is_valid());
    if (with.albedo.is_valid()) {
        out->set_shader_parameter("proj_albedo_tex", with.albedo);
    }
    out->set_shader_parameter("proj_params", with.params);
    out->set_shader_parameter("proj_direction", with.direction);
    out->set_shader_parameter("proj_color", with.color);
    out->set_shader_parameter("proj_normal_dampener", with.normal_dampener);
    projected_.emplace(id, out);
    return out;
}

void SkydotMaterials::configure_effect(const Ref<godot::ShaderMaterial>& out,
                                       const Dictionary& extras, bool double_sided,
                                       bool particles, const Ref<godot::Texture>& source,
                                       Color emission) {
    const std::uint32_t flags1 = as_u32(extras, "shader_flags1");
    const std::uint32_t flags2 = as_u32(extras, "shader_flags2");
    const std::uint32_t alpha_flags = as_u32(extras, "alpha_flags");
    const bool blend = (alpha_flags & k_alpha_blend) != 0;
    const bool test = (alpha_flags & k_alpha_test) != 0;
    const std::uint32_t destination = (alpha_flags >> 5) & 0xFu;
    const Alpha alpha = blend ? (multiplies(alpha_flags) ? Alpha::mul : destination == 0 ? Alpha::add : Alpha::blend)
                              : (test ? Alpha::test : Alpha::none);
    const Ref<godot::Texture> palette = load_texture(slot_path(extras, 3));
    const bool lit = (flags2 & k_sf2_effect_lighting) != 0;
    out->set_shader(shader_for(effect_code(double_sided, alpha, particles, lit)));
    out->set_shader_parameter("palette_color", (flags1 & k_sf1_greyscale_to_palette_color) != 0 &&
                                                   palette.is_valid());
    out->set_shader_parameter("palette_alpha", (flags1 & k_sf1_greyscale_to_palette_alpha) != 0 &&
                                                   palette.is_valid());
    // Particle colour and fade arrive as the instance colour.
    out->set_shader_parameter("use_vertex_colors", particles || (flags2 & k_sf2_vertex_colors) != 0);
    out->set_shader_parameter("use_vertex_alpha", particles || (flags1 & k_sf1_vertex_alpha) != 0);
    out->set_shader_parameter("use_falloff", (flags1 & k_sf1_use_falloff) != 0);
    out->set_shader_parameter("soft_effect", (flags1 & k_sf1_soft_effect) != 0 && blend);
    out->set_shader_parameter("source_tex", source);
    if (palette.is_valid()) {
        out->set_shader_parameter("palette_tex", palette);
    }
    if (emission == Color(0, 0, 0)) {
        emission = Color(1, 1, 1);
    }
    emission.a = static_cast<float>(as_double(extras, "emissive_alpha", 1.0));
    out->set_shader_parameter("emission_color", shader_rgba(emission));
    const godot::Vector3 f01 = as_vec3(extras, "falloff", godot::Vector3(1, 1, 0));
    const Variant falloff = extras.get("falloff", Variant());
    const double stop_opacity = falloff.get_type() == Variant::ARRAY && Array(falloff).size() >= 4
                                    ? static_cast<double>(Array(falloff)[3])
                                    : 0.0;
    out->set_shader_parameter("falloff_params",
                              godot::Vector4(f01.x, f01.y, f01.z, static_cast<float>(stop_opacity)));
    out->set_shader_parameter("emission_strength", as_double(extras, "emissive_multiple", 1.0));
    out->set_shader_parameter("soft_depth", as_double(extras, "soft_falloff_depth", 10.0) *
                                                formats::k_metres_per_unit);
    out->set_shader_parameter("uv_scale", as_vec2(extras, "uv_scale", godot::Vector2(1, 1)));
    out->set_shader_parameter("uv_offset", as_vec2(extras, "uv_offset", godot::Vector2(0, 0)));
}

Ref<godot::ShaderMaterial> SkydotMaterials::particle_material(const Dictionary& extras) {
    Ref<godot::ShaderMaterial> out;
    out.instantiate();
    const godot::Vector3 emissive = as_vec3(extras, "emissive_color", godot::Vector3(1, 1, 1));
    configure_effect(out, extras, true, true, load_texture(slot_path(extras, 0)),
                     Color(emissive.x, emissive.y, emissive.z));
    const std::uint32_t alpha_flags = as_u32(extras, "alpha_flags");
    if ((alpha_flags & k_alpha_test) != 0) {
        out->set_shader_parameter("alpha_cutoff", as_double(extras, "alpha_cutoff", 0.5));
    }
    return out;
}

namespace {

std::string utf8(const String& s) {
    const auto bytes = s.utf8();
    return std::string(bytes.get_data(), static_cast<std::size_t>(bytes.length()));
}

} // namespace

std::shared_ptr<const EffectAsset> SkydotMaterials::effects(godot::Node* root) {
    const String path = root->get_scene_file_path();
    if (path.is_empty()) {
        return EffectAsset::parse(root);
    }
    const std::string key = utf8(path);
    if (auto it = effects_.find(key); it != effects_.end()) {
        return it->second;
    }
    auto asset = EffectAsset::parse(root);
    effects_.emplace(key, asset);
    return asset;
}

Ref<godot::ShaderMaterial> SkydotMaterials::particle_material_for(const String& scene,
                                                                  std::int64_t index,
                                                                  const Dictionary& block) {
    const std::string key = utf8(scene) + "#" + std::to_string(index);
    if (!scene.is_empty()) {
        if (auto it = particle_materials_.find(key); it != particle_materials_.end()) {
            return it->second;
        }
    }
    const Variant extras = block.get("material", Variant());
    Ref<godot::ShaderMaterial> material =
        particle_material(extras.get_type() == Variant::DICTIONARY ? Dictionary(extras) : Dictionary());
    const Variant rects = block.get("subtex_offsets", Variant());
    if (rects.get_type() == Variant::ARRAY) {
        const Array list = rects;
        godot::PackedVector4Array packed;
        for (std::int64_t i = 0; i < list.size() && i < 64; ++i) {
            const Array r = list[i];
            if (r.size() >= 4) {
                packed.push_back(godot::Vector4(static_cast<float>(static_cast<double>(r[0])),
                                                static_cast<float>(static_cast<double>(r[1])),
                                                static_cast<float>(static_cast<double>(r[2])),
                                                static_cast<float>(static_cast<double>(r[3]))));
            }
        }
        if (!packed.is_empty()) {
            const std::int64_t count = packed.size();
            while (packed.size() < 64) {
                packed.push_back(godot::Vector4());
            }
            material->set_shader_parameter("subtex_rects", packed);
            material->set_shader_parameter("subtex_count", count);
        }
    }
    if (!scene.is_empty()) {
        particle_materials_.emplace(key, material);
    }
    return material;
}

Ref<godot::Shader> SkydotMaterials::particles_process_shader() {
    // Kept with the variants (warm_up), under its file name: the stubs' keys
    // are code, which starts with shader_type.
    Ref<godot::Shader>& shader = shaders_["particles_process.gdshader"];
    if (shader.is_null()) {
        shader = shader_source::shader("particles_process.gdshader");
    }
    return shader;
}

Dictionary SkydotMaterials::shader_sources() {
    Dictionary out;
    const auto add = [&out](const std::string& name, const std::string& code) {
        out[String::utf8(name.c_str(), static_cast<int>(name.size()))] =
            String::utf8(code.c_str(), static_cast<int>(code.size()));
    };
    const std::pair<Alpha, const char*> alphas[] = {
        {Alpha::none, "none"}, {Alpha::test, "test"}, {Alpha::blend, "blend"}, {Alpha::add, "add"}, {Alpha::mul, "mul"}};
    for (const bool double_sided : {false, true}) {
        const std::string side = double_sided ? "double" : "single";
        for (const auto& [alpha, alpha_name] : alphas) {
            add("lighting_" + side + "_" + alpha_name, lighting_code(double_sided, alpha));
            for (const bool particles : {false, true}) {
                for (const bool lit : {false, true}) {
                    add("effect_" + side + "_" + alpha_name + (particles ? "_particles" : "") + (lit ? "_lit" : "_unlit"),
                        effect_code(double_sided, alpha, particles, lit));
                }
            }
        }
        add("refraction_" + side, refraction_code(double_sided));
    }
    out["particles_process"] = shader_source::code("particles_process.gdshader");
    out.merge(TerrainBuilder::shader_codes());
    out.merge(WaterMaterials::shader_codes());
    out.merge(SkydotLod::shader_codes());
    out.merge(SkydotWeather::shader_codes());
    out.merge(SkydotImageSpace::shader_codes());
    return out;
}

std::int64_t SkydotMaterials::warm_up() {
    for (const bool double_sided : {false, true}) {
        for (const Alpha alpha : {Alpha::none, Alpha::test, Alpha::blend, Alpha::mul}) {
            shader_for(lighting_code(double_sided, alpha));
        }
        for (const bool lit : {false, true}) {
            for (const Alpha alpha : {Alpha::none, Alpha::test, Alpha::blend, Alpha::add, Alpha::mul}) {
                shader_for(effect_code(double_sided, alpha, false, lit));
                if (double_sided) {
                    shader_for(effect_code(true, alpha, true, lit));
                }
            }
        }
        shader_for(refraction_code(double_sided));
    }
    particles_process_shader();
    // Compiling happens when a material first uses a shader, not when the
    // shader is created: give each variant a material and keep it.
    for (const auto& [code, shader] : shaders_) {
        Ref<godot::ShaderMaterial> material;
        material.instantiate();
        material->set_shader(shader);
        material->set_shader_parameter("uv_scale", godot::Vector2(1, 1));
        warm_.push_back(material);
    }
    return static_cast<std::int64_t>(shaders_.size());
}

namespace {

/// The NIF shape has no BSShaderProperty at all (bethconv writes shader "none").
bool has_no_shader_property(const Ref<godot::Material>& source) {
    if (source.is_null() || !source->has_meta("extras")) {
        return false;
    }
    const Variant extras = source->get_meta("extras");
    if (extras.get_type() != Variant::DICTIONARY) {
        return false;
    }
    const Variant block = Dictionary(extras).get("bethconv", Variant());
    if (block.get_type() != Variant::DICTIONARY) {
        return false;
    }
    return Dictionary(block).get("shader", String()) == Variant(String("none"));
}

} // namespace

std::int64_t SkydotMaterials::apply(godot::Node* root) {
    if (root == nullptr) {
        return 0;
    }
    godot::TypedArray<godot::Node> meshes;
    collect_meshes(root, meshes);
    std::int64_t converted = 0;
    for (int i = 0; i < meshes.size(); ++i) {
        auto* instance = godot::Object::cast_to<godot::MeshInstance3D>(meshes[i]);
        const Ref<godot::Mesh> mesh = instance->get_mesh();
        if (mesh.is_null()) {
            continue;
        }
        // A shape with no shader property is not drawn by the game (the water
        // current planes of FXWaterfallBodySlope are such shapes); drawing
        // them as default lit white laid opaque white sheets over rapids.
        if (mesh->get_surface_count() > 0) {
            bool all_unshaded = true;
            for (int s = 0; s < mesh->get_surface_count() && all_unshaded; ++s) {
                all_unshaded = has_no_shader_property(mesh->surface_get_material(s));
            }
            if (all_unshaded) {
                instance->set_visible(false);
                continue;
            }
        }
        for (int s = 0; s < mesh->get_surface_count(); ++s) {
            const Ref<godot::Material> replacement = convert(mesh->surface_get_material(s));
            if (replacement.is_valid()) {
                instance->set_surface_override_material(s, replacement);
                ++converted;
            }
        }
    }
    return converted;
}

} // namespace skydot
