/* The terrain shader: game-style layers blended per vertex (see render/terrain.hpp).
 *
 * TerrainBuilder::shader_code() in render/terrain.cpp makes one variant per layer
 * count (1 to 7): it replaces the line %LAYER_SAMPLERS% with an albedo and a normal
 * sampler per layer, and %LAYER_MIXES% with a mix of albedo and normal per layer
 * after the first (weights_a holds layers 1-4, weights_b 5-6), then with_game_fog()
 * inserts game_fog.gdshaderinc after the render_mode line and ends fragment() with a
 * FOG line, and with_game_ambient() adds ambient_light_disabled to the render modes
 * and an EMISSION line at the end of fragment().
 *
 * This comment is not part of the shader code: the loader cuts it off (README.md).
 */
shader_type spatial;
render_mode cull_back, diffuse_lambert;

// Game-style layering: each additional layer is mixed over the result by its
// per-vertex opacity (CUSTOM0 holds layers 1-4, CUSTOM1 layers 5-6).
uniform float tiling = 8.0;
varying vec4 weights_a;
varying vec2 weights_b;
%LAYER_SAMPLERS%

void vertex() {
	weights_a = CUSTOM0;
	weights_b = CUSTOM1.xy;
}

void fragment() {
	vec2 uv = UV * tiling;
	vec3 albedo = texture(albedo_0, uv).rgb;
	vec3 n = texture(normal_0, uv).rgb;
%LAYER_MIXES%
	ALBEDO = albedo * COLOR.rgb;
	// Skyrim normal maps are DirectX-style (green points down).
	NORMAL_MAP = vec3(n.r, 1.0 - n.g, n.b);
	ROUGHNESS = 1.0;
	SPECULAR = 0.2;
}
