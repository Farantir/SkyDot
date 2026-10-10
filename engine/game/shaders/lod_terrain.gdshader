/* Terrain LOD shader (see render/lod.hpp).
 *
 * Terrain LOD: one texture per quad and a model-space normal map in the NIF's
 * axes (x east, y north, z up); the node transform carries it to Godot's.
 * The map's channels are not in that order: it stores the NIF normal as
 * (x, y, z) = (red, blue, green) * 2 - 1, "up" in green. Measured on the
 * .btr meshes of tamriel.4.0.-12, 4.-4.-12, 8.0.-16, 4.8.-4 and 4.16.-8:
 * of all 48 channel permutations and signs, this one has the highest mean dot
 * product with the mesh's vertex normals (0.79 to 0.91; the runner-up, mostly
 * (-b, -r, g), 0.57 to 0.74), with no sign flipped. Flipping the texture's v
 * lowers it to 0.4 to 0.6.
 * It is not higher because the mesh is a decimated terrain and the map is
 * baked from the full one.
 *
 * Detail: the game lays textures/terrain/noise.dds (grey, mean 0.215, tiling) over
 * terrain LOD and darkens it on the way (Lighting.hlsl LODLANDNOISE,
 * GetLodLandBlendParameter / GetLodLandBlendMultiplier): albedo x
 * 0.8333 * (b * (0.37 - noise) + noise) + 0.37, noise sampled at the quad's
 * UV x 3, b = 0.8 x smoothstep of the albedo's brightness (snow). On dark land
 * that is about 0.55 on average, on snow about 0.65: why the LOD textures are
 * brighter than the full terrain's. Measured 2026-10-10 on game shot
 * lodnovl_171528 (volumetric lighting off): far plain 114/126/115 -> 82/101/101,
 * game 78/90/81.
 *
 * A whole shader that SkydotLod loads as it is. It discards over loaded cells
 * (lod_mask.gdshaderinc), is lit by the game's ambient and has its fog (game_fog.gdshaderinc).
 */
shader_type spatial;
render_mode diffuse_lambert, cull_back, depth_draw_opaque, ambient_light_disabled;

#include "res://shaders/game_fog.gdshaderinc"
#include "res://shaders/lod_mask.gdshaderinc"

uniform sampler2D albedo_tex : filter_linear_mipmap_anisotropic, repeat_disable;
uniform sampler2D normal_tex : filter_linear_mipmap, repeat_disable;
uniform bool has_normal = false;
uniform sampler2D noise_tex : filter_linear_mipmap, repeat_enable;
uniform bool has_noise = false;

void vertex() {
	skydot_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
	if (skydot_masked(skydot_world)) {
		discard;
	}
	ALBEDO = texture(albedo_tex, UV).rgb;
	if (has_noise) {
		// Lighting.hlsl LODLANDNOISE: GetLodLandBlendParameter and
		// GetLodLandBlendMultiplier, the noise sampled at the quad's UV x 3.
		float noise = texture(noise_tex, UV * 3.0).r;
		float bright = clamp(1.6666666 * (dot(ALBEDO, vec3(0.55)) - 0.4), 0.0, 1.0);
		bright = bright * bright * (3.0 - 2.0 * bright) * 0.8;
		ALBEDO *= 0.8333333 * (bright * (0.37 - noise) + noise) + 0.37;
	}
	if (has_normal) {
		vec3 t = texture(normal_tex, UV).rgb * 2.0 - 1.0;
		vec3 n = vec3(t.r, t.b, t.g);
		NORMAL = normalize((VIEW_MATRIX * (MODEL_MATRIX * vec4(n, 0.0))).xyz);
	}
	ROUGHNESS = 1.0;
	SPECULAR = 0.1;
	FOG = skydot_game_fog(VERTEX);
	EMISSION += ALBEDO * skydot_land_ambient((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
}
