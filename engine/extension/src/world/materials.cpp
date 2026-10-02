// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/materials.hpp"

#include "world/effect_asset.hpp"
#include "world/particles.hpp"
#include "world/world.hpp"

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

#include <mutex>

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

constexpr const char* k_lighting_body = R"(
uniform sampler2D albedo_tex : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D normal_tex : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform vec4 base_color : source_color = vec4(1.0);
uniform vec3 specular_color = vec3(1.0);
uniform float specular_strength = 1.0;
uniform float glossiness = 30.0;
uniform vec3 emission_color : source_color = vec3(0.0);
uniform float emission_strength = 1.0;
uniform vec2 uv_scale = vec2(1.0);
uniform vec2 uv_offset = vec2(0.0);
uniform float alpha_cutoff = 0.5;
uniform bool blend_test = false; // blended, and below alpha_cutoff discarded
// Features are uniforms rather than #defines, so the number of shader
// variants (each compiled once, up front) stays small.
uniform bool use_vertex_colors = false;
uniform bool use_vertex_alpha = false;
uniform bool own_emit = false;
uniform bool use_glow_map = false;
uniform sampler2D glow_tex : source_color, filter_linear_mipmap, repeat_enable;
uniform bool use_env_map = false;
uniform bool use_env_mask = false;
uniform samplerCube env_tex : source_color, filter_linear_mipmap;
uniform sampler2D env_mask_tex : filter_linear_mipmap, repeat_enable;
uniform float env_scale = 1.0;
// Model-space normal maps (bodies, heads): the normal in the NIF's axes is
// (r, b, g) * 2 - 1, measured against face normals on malebody_1 (mean dot
// 0.98). Such shapes have no vertex normals. The specular mask is then its
// own texture (slot 7, `_s`). Skinned shapes use the bind pose's axes, so
// limbs bent far from it are lit as if they were not.
uniform bool model_space_normals = false;
uniform bool use_spec_tex = false;
uniform sampler2D spec_tex : filter_linear_mipmap, repeat_enable;
// SkinTint (shader type 5): the actor's skin tone, and FaceGen (type 4): the
// NPC's tint mask (slot 6), both soft-lit onto the texture as the game does,
// in gamma space: base^2 + 2 tint base (1 - base); a tint of 0.5 keeps it.
// Hence no source_color hints: the values stay in gamma space.
uniform bool use_skin_tint = false;
uniform vec3 skin_tint = vec3(0.5);
uniform bool use_face_tint = false;
uniform sampler2D face_tint_tex : filter_linear_mipmap, repeat_enable;
// HairTint (type 6): the hair colour, weighted by the vertex colours' green;
// hair takes no other vertex colour (its red and blue would darken it).
uniform bool use_hair_tint = false;
uniform vec3 hair_tint = vec3(1.0);

varying float spec_mask;

vec3 soft_tint(vec3 linear_base, vec3 tint) {
	vec3 base = pow(linear_base, vec3(1.0 / 2.2));
	return pow(clamp(base * base + 2.0 * tint * base * (1.0 - base), 0.0, 1.0), vec3(2.2));
}

void fragment() {
	vec2 uv = UV * uv_scale + uv_offset;
	vec4 albedo = texture(albedo_tex, uv) * base_color;
	if (use_vertex_colors) {
		albedo.rgb *= COLOR.rgb;
	}
	if (use_vertex_alpha) {
		albedo.a *= COLOR.a;
	}
	if (use_skin_tint) {
		albedo.rgb = soft_tint(albedo.rgb, skin_tint);
	}
	if (use_face_tint) {
		albedo.rgb = soft_tint(albedo.rgb, texture(face_tint_tex, uv).rgb);
	}
	if (use_hair_tint) {
		albedo.rgb *= pow(mix(vec3(1.0), hair_tint, COLOR.g), vec3(2.2));
	}
	vec4 n = texture(normal_tex, uv);
	if (model_space_normals) {
		vec3 m = n.rgb * 2.0 - 1.0;
		NORMAL = normalize((VIEW_MATRIX * (MODEL_MATRIX * vec4(m.r, m.b, m.g, 0.0))).xyz);
		spec_mask = use_spec_tex ? texture(spec_tex, uv).r : 0.0;
	} else {
		// Skyrim normal maps use the DirectX convention (green points down).
		NORMAL_MAP = vec3(n.r, 1.0 - n.g, n.b);
		spec_mask = use_spec_tex ? texture(spec_tex, uv).r : n.a;
	}
	METALLIC = 0.0;
	ROUGHNESS = 1.0;
	SPECULAR = 0.0;
	if (own_emit) {
		// The game lights the texture with it (albedo * (diffuse + emissive)
		// in NifSkope's sk_default.frag) rather than painting it flat.
		EMISSION = albedo.rgb * emission_color * emission_strength;
	}
	if (use_glow_map) {
		EMISSION = texture(glow_tex, uv).rgb * emission_color * emission_strength;
	}
	if (use_env_map) {
		// Added to the albedo, so lit like it; masked by the environment mask
		// or, without one, the specular mask (NifSkope's sk_default.frag).
		vec3 world_normal = normalize((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
		vec3 world_view = normalize((INV_VIEW_MATRIX * vec4(VIEW, 0.0)).xyz);
		vec3 reflected = reflect(-world_view, world_normal);
		float env_mask = use_env_mask ? texture(env_mask_tex, uv).r : n.a;
		albedo.rgb += texture(env_tex, reflected).rgb * env_scale * env_mask;
	}
	ALBEDO = albedo.rgb;
#ifdef ALPHA_TEST
	ALPHA = albedo.a;
	ALPHA_SCISSOR_THRESHOLD = alpha_cutoff;
#endif
#ifdef ALPHA_BLEND
	if (blend_test && albedo.a <= alpha_cutoff) {
		discard;
	}
	ALPHA = albedo.a;
#endif
}

void light() {
	float ndotl = clamp(dot(NORMAL, LIGHT), 0.0, 1.0);
	DIFFUSE_LIGHT += ndotl * ATTENUATION * LIGHT_COLOR / PI;
	vec3 half_vector = normalize(VIEW + LIGHT);
	float highlight = pow(clamp(dot(NORMAL, half_vector), 0.0, 1.0), max(glossiness, 1.0));
	SPECULAR_LIGHT += highlight * spec_mask * specular_strength * specular_color
			* ndotl * ATTENUATION * LIGHT_COLOR / PI;
}
)";

constexpr const char* k_effect_body = R"(
// Skyrim computes and blends effects in gamma space, Godot in linear space.
// Colours here stay in gamma space (no source_color hints) and are linearized
// at the end; additive output is premultiplied first, which matches the game
// exactly over dark backgrounds.
uniform sampler2D source_tex : filter_linear_mipmap, repeat_enable;
uniform sampler2D palette_tex : filter_linear, repeat_disable;
uniform sampler2D depth_tex : hint_depth_texture;
uniform vec4 emission_color = vec4(1.0);
uniform float emission_strength = 1.0;
uniform vec4 falloff_params = vec4(1.0, 1.0, 0.0, 0.0);
uniform float soft_depth = 0.14;
uniform vec2 uv_scale = vec2(1.0);
uniform vec2 uv_offset = vec2(0.0);
uniform float alpha_cutoff = 0.5;
uniform bool use_vertex_colors = false;
uniform bool use_vertex_alpha = false;
uniform bool use_falloff = false;
uniform bool palette_color = false;
uniform bool palette_alpha = false;
uniform bool soft_effect = false;
#ifdef ALPHA_ADD
global uniform vec4 skydot_fog;
#endif

#ifdef PARTICLES
// Camera-facing quads sized by the particle transform's scale and turned by
// INSTANCE_CUSTOM.x; INSTANCE_CUSTOM.z picks a sub-texture rectangle
// (u, width, v, height).
uniform vec4 subtex_rects[64];
uniform int subtex_count = 0;
varying flat vec4 subtex_rect;

void vertex() {
	mat4 world = mat4(normalize(INV_VIEW_MATRIX[0]), normalize(INV_VIEW_MATRIX[1]),
			normalize(INV_VIEW_MATRIX[2]), MODEL_MATRIX[3]);
	float angle = INSTANCE_CUSTOM.x;
	world = world * mat4(vec4(cos(angle), -sin(angle), 0.0, 0.0),
			vec4(sin(angle), cos(angle), 0.0, 0.0), vec4(0.0, 0.0, 1.0, 0.0), vec4(0.0, 0.0, 0.0, 1.0));
	float size = length(MODEL_MATRIX[0].xyz);
	MODELVIEW_MATRIX = VIEW_MATRIX * world * mat4(vec4(size, 0.0, 0.0, 0.0),
			vec4(0.0, size, 0.0, 0.0), vec4(0.0, 0.0, size, 0.0), vec4(0.0, 0.0, 0.0, 1.0));
	MODELVIEW_NORMAL_MATRIX = mat3(MODELVIEW_MATRIX);
	if (subtex_count > 0) {
		vec4 r = subtex_rects[clamp(int(INSTANCE_CUSTOM.z), 0, subtex_count - 1)];
		UV = vec2(r.x + UV.x * r.y, r.z + UV.y * r.w);
		subtex_rect = r;
	} else {
		subtex_rect = vec4(0.0, 1.0, 0.0, 1.0);
	}
}
#endif

#ifdef LIT
// Effect_Lighting: the scene's lights tint the effect (mountain clouds take the
// weather's ambient and sun) instead of it glowing at full strength. Wrapped,
// so the side away from the sun is dimmer but not black.
void light() {
	float wrap = clamp(dot(NORMAL, LIGHT) * 0.5 + 0.5, 0.0, 1.0);
	DIFFUSE_LIGHT += wrap * ATTENUATION * LIGHT_COLOR / PI;
}
#endif

vec3 to_linear(vec3 c) {
	c = max(c, vec3(0.0));
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

// Follows the game's effect pixel shader as reverse-engineered by Community
// Shaders (Effect.hlsl); NifSkope's version leaves the texture alpha out of the
// palette row.
void fragment() {
	vec2 source_uv = UV * uv_scale + uv_offset;
	vec4 source = texture(source_tex, source_uv);
#ifdef PARTICLES
	// A sub-texture is one cell of an atlas, and the game's own small mips
	// blur the cells into each other: from 16 texels a cell down, its
	// transparent border fills in and far particles turn into squares.
	vec2 cell = subtex_rect.yw * vec2(textureSize(source_tex, 0));
	float max_lod = max(log2(max(min(cell.x, cell.y), 1.0)) - 4.0, 0.0);
	source = textureLod(source_tex, source_uv, min(textureQueryLod(source_tex, source_uv).y, max_lod));
#endif
	vec4 vertex = vec4(1.0);
	if (use_vertex_colors) {
		vertex.rgb = COLOR.rgb;
	}
	if (use_vertex_alpha) {
		vertex.a = COLOR.a;
	}
	float falloff = 1.0;
	if (use_falloff) {
		// x/y: start/stop angle (cosines), z/w: start/stop opacity.
		falloff = smoothstep(falloff_params.y, falloff_params.x, abs(dot(NORMAL, VIEW)));
		falloff = mix(max(falloff_params.w, 0.0), min(falloff_params.z, 1.0), falloff);
	}
	float alpha_mult = emission_color.a * emission_color.a;
	vec3 color = source.rgb * vertex.rgb * emission_color.rgb;
	float alpha = source.a * vertex.a * falloff * alpha_mult;
	if (soft_effect) {
		// Fade where the surface nears the geometry behind it.
		float depth = texture(depth_tex, SCREEN_UV).r;
		vec4 behind = INV_PROJECTION_MATRIX * vec4(SCREEN_UV * 2.0 - 1.0, depth, 1.0);
		alpha *= clamp((VERTEX.z - behind.z / behind.w) / soft_depth, 0.0, 1.0);
	}
	if (palette_color) {
		color = texture(palette_tex, vec2(source.g,
				clamp(vertex.r * emission_color.r, 0.0, 1.0))).rgb;
	}
	if (palette_alpha) {
		// The row is the whole alpha so far, the texture's included, so
		// blurred mips of thin detail stay near the transparent corner.
		alpha = texture(palette_tex, vec2(source.a, clamp(alpha, 0.0, 1.0))).a;
	}
	color *= emission_strength;
#ifdef ALPHA_ADD
	ALBEDO = to_linear(color * alpha);
	ALPHA = 1.0;
	// Fog would mix the transparent (black) parts towards its colour, and
	// adding that draws every quad as a square far away. Fade towards black
	// by Godot's depth fog amount instead (SkydotMaterials::sync_fog).
	float fog_far = skydot_fog.y > skydot_fog.x ? skydot_fog.y : skydot_fog.x + 1.0;
	float fog_z = smoothstep(skydot_fog.x, fog_far, length(VERTEX));
	FOG = vec4(0.0, 0.0, 0.0, clamp(pow(fog_z, skydot_fog.z) * skydot_fog.w, 0.0, 1.0));
#else
	ALBEDO = to_linear(color);
#endif
#ifdef LIT
	ROUGHNESS = 1.0;
	SPECULAR = 0.0;
#endif
#ifdef ALPHA_TEST
	ALPHA = alpha;
	ALPHA_SCISSOR_THRESHOLD = alpha_cutoff;
#endif
#ifdef ALPHA_BLEND
	ALPHA = alpha;
#endif
}
)";

constexpr const char* k_refraction_body = R"(
uniform sampler2D normal_tex : hint_normal, filter_linear_mipmap, repeat_enable;
uniform sampler2D screen_tex : hint_screen_texture, filter_linear_mipmap;
uniform vec2 uv_scale = vec2(1.0);
uniform vec2 uv_offset = vec2(0.0);
uniform float strength = 0.004;

void fragment() {
	vec3 n = texture(normal_tex, UV * uv_scale + uv_offset).rgb * 2.0 - 1.0;
	ALBEDO = textureLod(screen_tex, SCREEN_UV + n.xy * strength, 0.0).rgb;
	ALPHA = 1.0;
}
)";

enum class Alpha { none, test, blend, add };

/// The shader code of each variant. Everything else is a uniform, so these
/// are all the shaders materials ever need (see SkydotMaterials::warm_up).
std::string lighting_code(bool double_sided, Alpha alpha) {
    std::string modes = double_sided ? "cull_disabled" : "cull_back";
    std::string defines;
    if (alpha == Alpha::test) {
        defines = "#define ALPHA_TEST\n";
    } else if (alpha == Alpha::blend) {
        defines = "#define ALPHA_BLEND\n";
        modes += ", blend_mix";
    }
    return "shader_type spatial;\nrender_mode " + modes + ";\n" + defines + k_lighting_body;
}

std::string effect_code(bool double_sided, Alpha alpha, bool particles = false, bool lit = false) {
    std::string modes = double_sided ? "cull_disabled" : "cull_back";
    modes += lit ? ", specular_disabled" : ", unshaded";
    std::string defines = particles ? "#define PARTICLES\n" : "";
    if (lit) {
        defines += "#define LIT\n";
    }
    if (alpha == Alpha::add) {
        modes += ", depth_draw_never, blend_add";
        defines += "#define ALPHA_ADD\n";
    } else if (alpha == Alpha::blend) {
        modes += ", depth_draw_never, blend_mix";
        defines += "#define ALPHA_BLEND\n";
    } else if (alpha == Alpha::test) {
        defines += "#define ALPHA_TEST\n";
    }
    return "shader_type spatial;\nrender_mode " + modes + ";\n" + defines + k_effect_body;
}

// The screen texture is already fogged; fogging the copy again drew a pale
// veil in the shape of heat-haze planes.
std::string refraction_code(bool double_sided) {
    return std::string("shader_type spatial;\nrender_mode ") +
           (double_sided ? "cull_disabled" : "cull_back") +
           ", unshaded, fog_disabled, depth_draw_never, blend_mix;\n" + k_refraction_body;
}

std::uint32_t as_u32(const Dictionary& d, const char* key) {
    const Variant v = d.get(key, 0);
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(static_cast<double>(v)));
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
    godot::ClassDB::bind_static_method(get_class_static(), D_METHOD("sync_fog", "environment"),
                                       &SkydotMaterials::sync_fog);
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
        godot::RenderingServer::get_singleton()->global_shader_parameter_add(
            "skydot_fog", godot::RenderingServer::GLOBAL_VAR_TYPE_VEC4, godot::Vector4(0, 1, 1, 0));
    });
}

void SkydotMaterials::sync_fog(const Ref<godot::Environment>& environment) {
    ensure_fog_globals();
    godot::Vector4 fog(0, 1, 1, 0);
    if (environment.is_valid() && environment->is_fog_enabled() &&
        environment->get_fog_mode() == godot::Environment::FOG_MODE_DEPTH) {
        fog = godot::Vector4(environment->get_fog_depth_begin(), environment->get_fog_depth_end(),
                             environment->get_fog_depth_curve(), environment->get_fog_density());
    }
    godot::RenderingServer::get_singleton()->global_shader_parameter_set("skydot_fog", fog);
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
        out->set_shader_parameter("base_color", Color(0.25F, 0.35F, 0.4F, 0.6F));
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
        out->set_shader_parameter("base_color", base->get_albedo());
        out->set_shader_parameter("specular_color",
                                  as_vec3(extras, "specular_color", godot::Vector3(1, 1, 1)));
        // Without the Specular flag the game draws no highlight at all.
        out->set_shader_parameter("specular_strength", (flags1 & k_sf1_specular) != 0
                                                           ? as_double(extras, "specular_strength", 1.0)
                                                           : 0.0);
        out->set_shader_parameter("glossiness", as_double(extras, "glossiness", 30.0));
        out->set_shader_parameter("emission_color", base->get_emission());
        out->set_shader_parameter("emission_strength", as_double(extras, "emissive_multiple", 1.0));
        out->set_shader_parameter("soft_depth", as_double(extras, "soft_falloff_depth", 10.0) *
                                                    SkydotWorld::UNIT_SCALE);
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
    const Alpha alpha = blend ? (destination == 0 ? Alpha::add : Alpha::blend)
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
    out->set_shader_parameter("emission_color", emission);
    const godot::Vector3 f01 = as_vec3(extras, "falloff", godot::Vector3(1, 1, 0));
    const Variant falloff = extras.get("falloff", Variant());
    const double stop_opacity = falloff.get_type() == Variant::ARRAY && Array(falloff).size() >= 4
                                    ? static_cast<double>(Array(falloff)[3])
                                    : 0.0;
    out->set_shader_parameter("falloff_params",
                              godot::Vector4(f01.x, f01.y, f01.z, static_cast<float>(stop_opacity)));
    out->set_shader_parameter("emission_strength", as_double(extras, "emissive_multiple", 1.0));
    out->set_shader_parameter("soft_depth", as_double(extras, "soft_falloff_depth", 10.0) *
                                                SkydotWorld::UNIT_SCALE);
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
    return shader_for(SkydotParticles::process_shader_code());
}

std::int64_t SkydotMaterials::warm_up() {
    for (const bool double_sided : {false, true}) {
        for (const Alpha alpha : {Alpha::none, Alpha::test, Alpha::blend}) {
            shader_for(lighting_code(double_sided, alpha));
        }
        for (const bool lit : {false, true}) {
            for (const Alpha alpha : {Alpha::none, Alpha::test, Alpha::blend, Alpha::add}) {
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
