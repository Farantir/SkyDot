/* Water LOD shader (see render/lod.hpp).
 *
 * A whole shader that SkydotLod loads as it is. It discards over loaded cells
 * (lod_mask.gdshaderinc), is lit by the game's ambient and has its fog (game_fog.gdshaderinc).
 */
shader_type spatial;
render_mode diffuse_lambert, cull_back, depth_draw_opaque, ambient_light_disabled;

#include "res://shaders/game_fog.gdshaderinc"
#include "res://shaders/lod_mask.gdshaderinc"

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
	FOG = skydot_game_fog(VERTEX);
	EMISSION += ALBEDO * skydot_ambient((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
}
