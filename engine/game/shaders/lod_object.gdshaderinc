/* Object LOD shader (see render/lod.hpp).
 *
 * Object LOD: the atlas (or a landscape texture for "HD" pieces) times the
 * vertex colour, and a tangent-space normal map in the DirectX convention.
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

uniform sampler2D albedo_tex : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D normal_tex : hint_normal, filter_linear_mipmap, repeat_enable;
uniform bool has_normal = false;

void vertex() {
	skydot_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
	if (skydot_masked(skydot_world)) {
		discard;
	}
	ALBEDO = texture(albedo_tex, UV).rgb * COLOR.rgb;
	if (has_normal) {
		vec4 n = texture(normal_tex, UV);
		NORMAL_MAP = vec3(n.r, 1.0 - n.g, n.b);
	}
	ROUGHNESS = 0.9;
	SPECULAR = 0.2;
}
