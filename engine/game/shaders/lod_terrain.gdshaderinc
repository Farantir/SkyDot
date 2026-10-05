/* Terrain LOD shader (see render/lod.hpp).
 *
 * Terrain LOD: one texture per quad and a model-space normal map in the NIF's
 * axes (x east, y north, z up); the node transform carries it to Godot's.
 *
 * lod_code() in render/lod.cpp builds the shader from
 *     shader_type spatial;
 *     render_mode diffuse_lambert, cull_back, depth_draw_opaque;
 * then lod_mask.gdshaderinc, then this file; then with_game_fog() inserts
 * game_fog.gdshaderinc after the render_mode line and ends fragment() with a FOG
 * line, and with_game_ambient() adds ambient_light_disabled to the render modes and
 * an EMISSION line at the end of fragment().
 *
 * This comment is not part of the shader code: the loader cuts it off (README.md).
 */

uniform sampler2D albedo_tex : filter_linear_mipmap_anisotropic, repeat_disable;
uniform sampler2D normal_tex : filter_linear_mipmap, repeat_disable;
uniform bool has_normal = false;

void vertex() {
	skydot_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
	if (skydot_masked(skydot_world)) {
		discard;
	}
	ALBEDO = texture(albedo_tex, UV).rgb;
	if (has_normal) {
		vec3 n = texture(normal_tex, UV).rgb * 2.0 - 1.0;
		NORMAL = normalize((VIEW_MATRIX * (MODEL_MATRIX * vec4(n, 0.0))).xyz);
	}
	ROUGHNESS = 1.0;
	SPECULAR = 0.1;
}
