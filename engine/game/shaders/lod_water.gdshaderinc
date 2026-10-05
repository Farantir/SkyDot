/* Water LOD shader (see render/lod.hpp).
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

uniform vec4 color = vec4(0.10, 0.17, 0.20, 1.0);

void vertex() {
	skydot_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
	if (skydot_masked(skydot_world)) {
		discard;
	}
	ALBEDO = color.rgb;
	ROUGHNESS = 0.1;
	SPECULAR = 0.6;
}
