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
#ifdef SKYDOT_ALPHA_TEST
render_mode alpha_to_coverage;
#endif

#include "res://shaders/game_fog.gdshaderinc"

uniform sampler2D albedo_tex : filter_linear_mipmap_anisotropic, repeat_enable;
// Foliage (alpha-tested trees, grass) samples its albedo this many mip levels
// blurrier: the alpha test of a thin leaf aliases at the level the pixel asks
// for, and a coarser one is steadier. The bias is one global (--foliage-bias),
// the flag is per material (materials.cpp).
global uniform float skydot_foliage_bias;
uniform bool foliage = false;
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
// falloff_bias), blended by smoothstep(0, 1, 5 (0.1 + weight)). The direction
// is in world space; the normal is the mapped one, as Community Shaders'
// worldNormal is (with the smooth vertex normal of a low-poly cliff the whole
// upward face took the material as one blob, where the game has snow settling
// on the normal map's detail).
// Single pass materials (proj_snow) use the engine's textures: the noise is
// projectednoise.dds sampled at the model-space position (xy, game units) over
// the material's noise scale, and the snow diffuse is laid over it at a fixed
// tiling (the game's INI default). Without those textures
// (proj_snow off) the snow is one colour, softened over a pixel (fwidth),
// with value noise on the world position.
uniform bool use_projection = false;
uniform bool proj_textured = false;
uniform sampler2D proj_albedo_tex : filter_linear_mipmap_anisotropic, repeat_enable;
uniform vec4 proj_params = vec4(0.0); // falloff scale, falloff bias, noise and texture repeats per metre
uniform vec3 proj_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 proj_color = vec3(1.0);
uniform float proj_normal_dampener = 0.0;
uniform bool proj_snow = false;
uniform float proj_noise_scale = 50.0; // game units per noise texture repeat
uniform sampler2D proj_noise_tex : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D proj_diffuse_tex : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D proj_normal_tex : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D proj_detail_tex : filter_linear_mipmap_anisotropic, repeat_enable;

varying float spec_mask;
varying vec3 model_pos; // the shape's own coordinates (the NIF's, in game units)

void vertex() {
	model_pos = VERTEX;
}

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
	vec4 albedo = texture(albedo_tex, uv, foliage ? skydot_foliage_bias : 0.0) * base_color;
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
	vec3 geo_normal = NORMAL;
	float snow_rim = 0.0;
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
		vec2 noise_uv = model_pos.xy / max(proj_noise_scale, 0.0001);
		float noise = proj_snow ? texture(proj_noise_tex, noise_uv).r : proj_noise(world_pos * proj_params.z);
		float weight = -proj_params.x * noise
				+ dot(world_normal, proj_direction) * COLOR.a - proj_params.y;
		if (proj_snow) {
			float t1 = clamp(5.0 * (0.1 + weight), 0.0, 1.0);
			float amount = t1 * t1 * (3.0 - 2.0 * t1);
			albedo.rgb = mix(albedo.rgb, texture(proj_diffuse_tex, 0.2 * noise_uv).rgb * proj_color, amount);
			// The snow's own normal map does not light it: Lighting.hlsl
			// lights with modelNormal, taken before the blend, so the snow
			// keeps the rock's mapped normal. The rim light (improved snow,
			// GetSnowSpecularColor; fSnowRimLightIntensity 0.3, geometry /
			// normal powers 3 / 2) goes to the snow buffer and is added in
			// image space where there is snow.
			snow_rim = 0.3 * amount * pow(1.0 - clamp(dot(geo_normal, VIEW), 0.0, 1.0), 3.0)
					* pow(1.0 - clamp(dot(NORMAL, VIEW), 0.0, 1.0), 2.0);
		} else if (proj_textured) {
			float x = 5.0 * (0.1 + weight);
			float w = max(1.0, fwidth(x));
			float t = clamp((x - 0.5) / w + 0.5, 0.0, 1.0);
			float amount = t * t * (3.0 - 2.0 * t);
			vec3 tri = pow(abs(world_normal), vec3(4.0));
			tri /= max(dot(tri, vec3(1.0)), 0.0001);
			vec3 p = world_pos * proj_params.w;
			vec3 snow = texture(proj_albedo_tex, p.zy).rgb * tri.x + texture(proj_albedo_tex, p.xz).rgb * tri.y
					+ texture(proj_albedo_tex, p.xy).rgb * tri.z;
			albedo.rgb = mix(albedo.rgb, snow * proj_color, amount);
			NORMAL = normalize(mix(NORMAL, normalize(NORMAL + (VIEW_MATRIX * vec4(proj_direction, 0.0)).xyz),
					amount * proj_normal_dampener));
		} else {
			float t = clamp(weight / max(fwidth(weight), 0.0001) + 0.5, 0.0, 1.0);
			albedo.rgb = mix(albedo.rgb, proj_color, t);
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
	ALPHA = clamp((albedo.a - alpha_cutoff) / max(fwidth(albedo.a), 0.0001) + 0.5, 0.0, 1.0);
	// Not the cutoff: that is in ALPHA already. This only keeps the pass from
	// drawing (and the shadow pass from casting) the fully clear texels.
	ALPHA_SCISSOR_THRESHOLD = 0.01;
#endif
#ifdef SKYDOT_ALPHA_BLEND
	if (blend_test && albedo.a <= alpha_cutoff) {
		discard;
	}
	ALPHA = albedo.a;
#endif
	FOG = skydot_game_fog(VERTEX);
	EMISSION += ALBEDO * skydot_ambient((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
	EMISSION += vec3(snow_rim);
}

void light() {
	float ndotl = clamp(dot(NORMAL, LIGHT), 0.0, 1.0);
	DIFFUSE_LIGHT += ndotl * ATTENUATION * LIGHT_COLOR / PI;
	vec3 half_vector = normalize(VIEW + LIGHT);
	float highlight = pow(clamp(dot(NORMAL, half_vector), 0.0, 1.0), max(glossiness, 1.0));
	SPECULAR_LIGHT += highlight * spec_mask * specular_strength * specular_color
			* ndotl * ATTENUATION * LIGHT_COLOR / PI;
}
