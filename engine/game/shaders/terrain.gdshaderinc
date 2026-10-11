/* The terrain shader: game-style layers blended per vertex (see render/terrain.hpp).
 *
 * Not a whole shader: TerrainBuilder::shader_code() in render/terrain.cpp makes a stub for each layer
 * count (1 to 7),
 *     shader_type spatial;
 *     #define SKYDOT_LAYERS 3
 *     #include "res://shaders/terrain.gdshaderinc"
 * and the #if blocks below declare a pair of samplers per layer and mix each layer after the first
 * (weights_a holds layers 1-4, weights_b 5-6). Lit by the game's ambient, with its fog
 * (game_fog.gdshaderinc).
 */
render_mode cull_back, diffuse_lambert, ambient_light_disabled;

#include "res://shaders/game_fog.gdshaderinc"

// Game-style layering: each additional layer adds its per-vertex opacity as a
// weight (CUSTOM0 holds layers 1-4, CUSTOM1 layers 5-6) and the base takes
// what is left, 1 - sum (weights renormalised when the sum exceeds 1). Not an
// "over" stack: the quadrants sharing a border vertex list different bases
// and layer orders, and only the sum makes them agree (no straight seams).
uniform float tiling = 24.0;
varying vec4 weights_a;
varying vec2 weights_b;
uniform sampler2D albedo_0 : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D normal_0 : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
#if SKYDOT_LAYERS > 1
uniform sampler2D albedo_1 : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D normal_1 : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
#endif
#if SKYDOT_LAYERS > 2
uniform sampler2D albedo_2 : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D normal_2 : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
#endif
#if SKYDOT_LAYERS > 3
uniform sampler2D albedo_3 : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D normal_3 : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
#endif
#if SKYDOT_LAYERS > 4
uniform sampler2D albedo_4 : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D normal_4 : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
#endif
#if SKYDOT_LAYERS > 5
uniform sampler2D albedo_5 : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D normal_5 : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
#endif
#if SKYDOT_LAYERS > 6
uniform sampler2D albedo_6 : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D normal_6 : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
#endif

void vertex() {
	weights_a = CUSTOM0;
	weights_b = CUSTOM1.xy;
}

void fragment() {
	vec2 uv = UV * tiling;
	vec3 albedo = texture(albedo_0, uv).rgb;
	vec3 n = texture(normal_0, uv).rgb;
	vec3 layer_albedo = vec3(0.0);
	vec3 layer_normal = vec3(0.0);
	float sum = 0.0;
#if SKYDOT_LAYERS > 1
	layer_albedo += texture(albedo_1, uv).rgb * weights_a[0];
	layer_normal += texture(normal_1, uv).rgb * weights_a[0];
	sum += weights_a[0];
#endif
#if SKYDOT_LAYERS > 2
	layer_albedo += texture(albedo_2, uv).rgb * weights_a[1];
	layer_normal += texture(normal_2, uv).rgb * weights_a[1];
	sum += weights_a[1];
#endif
#if SKYDOT_LAYERS > 3
	layer_albedo += texture(albedo_3, uv).rgb * weights_a[2];
	layer_normal += texture(normal_3, uv).rgb * weights_a[2];
	sum += weights_a[2];
#endif
#if SKYDOT_LAYERS > 4
	layer_albedo += texture(albedo_4, uv).rgb * weights_a[3];
	layer_normal += texture(normal_4, uv).rgb * weights_a[3];
	sum += weights_a[3];
#endif
#if SKYDOT_LAYERS > 5
	layer_albedo += texture(albedo_5, uv).rgb * weights_b[0];
	layer_normal += texture(normal_5, uv).rgb * weights_b[0];
	sum += weights_b[0];
#endif
#if SKYDOT_LAYERS > 6
	layer_albedo += texture(albedo_6, uv).rgb * weights_b[1];
	layer_normal += texture(normal_6, uv).rgb * weights_b[1];
	sum += weights_b[1];
#endif
	float base = max(1.0 - sum, 0.0);
	float total = base + sum;
	albedo = (albedo * base + layer_albedo) / total;
	n = (n * base + layer_normal) / total;
	ALBEDO = albedo * COLOR.rgb;
	// Skyrim normal maps are DirectX-style (green points down).
	NORMAL_MAP = vec3(n.r, 1.0 - n.g, n.b);
	ROUGHNESS = 1.0;
	SPECULAR = 0.2;
	FOG = skydot_game_fog(VERTEX);
	EMISSION += ALBEDO * skydot_land_ambient((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
}
