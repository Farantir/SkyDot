/* The lighting shader: BSLightingShaderProperty (see render/materials.hpp).
 *
 * Not a whole shader: lighting_code() in render/materials.cpp makes a stub for each variant,
 *     shader_type spatial;
 *     [#define SKYDOT_DOUBLE_SIDED]
 *     [#define SKYDOT_ALPHA_TEST | #define SKYDOT_ALPHA_BLEND | #define SKYDOT_ALPHA_MUL]
 *     #include "res://shaders/lighting.gdshaderinc"
 * and the render modes below follow from the defines. The game's fog is written to FOG,
 * and the game's directional ambient replaces Godot's (game_fog.gdshaderinc).
 */
#ifdef SKYDOT_DOUBLE_SIDED
render_mode cull_disabled;
#else
render_mode cull_back;
#endif
#if defined(SKYDOT_ALPHA_BLEND)
render_mode blend_mix;
#elif defined(SKYDOT_ALPHA_MUL)
render_mode depth_draw_never, blend_mul, fog_disabled;
#endif
render_mode ambient_light_disabled;

#include "res://shaders/game_fog.gdshaderinc"

uniform sampler2D albedo_tex : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D normal_tex : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform vec4 base_color = vec4(1.0);
uniform vec3 specular_color = vec3(1.0);
uniform float specular_strength = 1.0;
uniform float glossiness = 30.0;
uniform vec3 emission_color = vec3(0.0);
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
uniform sampler2D glow_tex : filter_linear_mipmap, repeat_enable;
uniform bool use_env_map = false;
uniform bool use_env_mask = false;
uniform samplerCube env_tex : filter_linear_mipmap;
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
uniform bool use_skin_tint = false;
uniform vec3 skin_tint = vec3(0.5);
uniform bool use_face_tint = false;
uniform sampler2D face_tint_tex : filter_linear_mipmap, repeat_enable;
// HairTint (type 6): the hair colour, weighted by the vertex colours' green;
// hair takes no other vertex colour (its red and blue would darken it).
uniform bool use_hair_tint = false;
uniform vec3 hair_tint = vec3(1.0);

// Directional material (STAT DNAM -> MATO) on Projected UV shapes: snow on
// what faces up. The game's weight (Community Shaders, Lighting.hlsl):
// -falloff_scale * noise + (dot(world normal, direction) * vertex alpha -
// falloff_bias), blended by smoothstep(0, 1, 5 (0.1 + weight)). The game's
// noise texture is the engine's own; value noise stands in.
uniform bool use_projection = false;
uniform bool proj_textured = false;
uniform sampler2D proj_albedo_tex : filter_linear_mipmap_anisotropic, repeat_enable;
uniform vec4 proj_params = vec4(0.0); // falloff scale, falloff bias, noise and texture repeats per metre
uniform vec3 proj_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 proj_color = vec3(1.0);
uniform float proj_normal_dampener = 0.0;

varying float spec_mask;

float proj_hash(vec3 p) {
	p = fract(p * 0.3183099 + 0.1);
	p *= 17.0;
	return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float proj_noise(vec3 x) {
	vec3 i = floor(x);
	vec3 f = fract(x);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(proj_hash(i), proj_hash(i + vec3(1, 0, 0)), f.x),
	               mix(proj_hash(i + vec3(0, 1, 0)), proj_hash(i + vec3(1, 1, 0)), f.x), f.y),
	           mix(mix(proj_hash(i + vec3(0, 0, 1)), proj_hash(i + vec3(1, 0, 1)), f.x),
	               mix(proj_hash(i + vec3(0, 1, 1)), proj_hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

vec3 soft_tint(vec3 base, vec3 tint) {
	return clamp(base * base + 2.0 * tint * base * (1.0 - base), 0.0, 1.0);
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
		albedo.rgb *= mix(vec3(1.0), hair_tint, COLOR.g);
	}
	vec4 n = texture(normal_tex, uv);
	if (model_space_normals) {
		vec3 m = n.rgb * 2.0 - 1.0;
		NORMAL = normalize((VIEW_MATRIX * (MODEL_MATRIX * vec4(m.r, m.b, m.g, 0.0))).xyz);
		spec_mask = use_spec_tex ? texture(spec_tex, uv).r : 0.0;
	} else {
		// Skyrim normal maps use the DirectX convention (green points down).
		// Applied here rather than through NORMAL_MAP, so the ambient
		// (skydot_ambient below) sees the mapped normal too.
		vec2 m = vec2(n.r * 2.0 - 1.0, 1.0 - n.g * 2.0);
		NORMAL = normalize(TANGENT * m.x + BINORMAL * m.y
				+ NORMAL * sqrt(max(0.0, 1.0 - dot(m, m))));
		spec_mask = use_spec_tex ? texture(spec_tex, uv).r : n.a;
	}
	if (use_projection) {
		vec3 world_pos = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
		vec3 world_normal = normalize((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
		float weight = -proj_params.x * proj_noise(world_pos * proj_params.z)
				+ dot(world_normal, proj_direction) * COLOR.a - proj_params.y;
		if (proj_textured) {
			float amount = smoothstep(0.0, 1.0, 5.0 * (0.1 + weight));
			vec3 tri = pow(abs(world_normal), vec3(4.0));
			tri /= max(dot(tri, vec3(1.0)), 0.0001);
			vec3 p = world_pos * proj_params.w;
			vec3 snow = texture(proj_albedo_tex, p.zy).rgb * tri.x + texture(proj_albedo_tex, p.xz).rgb * tri.y
					+ texture(proj_albedo_tex, p.xy).rgb * tri.z;
			albedo.rgb = mix(albedo.rgb, snow * proj_color, amount);
			NORMAL = normalize(mix(NORMAL, normalize(NORMAL + (VIEW_MATRIX * vec4(proj_direction, 0.0)).xyz),
					amount * proj_normal_dampener));
		} else if (weight > 0.0) {
			albedo.rgb = proj_color;
		}
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
		// Like own emit, the game adds the glow to the light the texture
		// takes (Community Shaders, Lighting.hlsl: diffuseColor += emitColor).
		EMISSION = albedo.rgb * texture(glow_tex, uv).rgb * emission_color * emission_strength;
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
#ifdef SKYDOT_ALPHA_TEST
	ALPHA = albedo.a;
	ALPHA_SCISSOR_THRESHOLD = alpha_cutoff;
#endif
#ifdef SKYDOT_ALPHA_BLEND
	if (blend_test && albedo.a <= alpha_cutoff) {
		discard;
	}
	ALPHA = albedo.a;
#endif
	FOG = skydot_game_fog(VERTEX);
	EMISSION += ALBEDO * skydot_ambient((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
}

void light() {
	float ndotl = clamp(dot(NORMAL, LIGHT), 0.0, 1.0);
	DIFFUSE_LIGHT += ndotl * ATTENUATION * LIGHT_COLOR / PI;
	vec3 half_vector = normalize(VIEW + LIGHT);
	float highlight = pow(clamp(dot(NORMAL, half_vector), 0.0, 1.0), max(glossiness, 1.0));
	SPECULAR_LIGHT += highlight * spec_mask * specular_strength * specular_color
			* ndotl * ATTENUATION * LIGHT_COLOR / PI;
}
