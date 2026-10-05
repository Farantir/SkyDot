/* The refraction shader: lighting shapes with SLSF1_REFRACTION (see render/materials.hpp).
 *
 * Not a whole shader: refraction_code() in render/materials.cpp makes a stub for each variant,
 *     shader_type spatial;
 *     [#define SKYDOT_DOUBLE_SIDED]
 *     #include "res://shaders/refraction.gdshaderinc"
 * It has no fog (the screen texture is already fogged; fogging the copy again drew a pale
 * veil in the shape of heat-haze planes) and no game_fog.gdshaderinc.
 */
#ifdef SKYDOT_DOUBLE_SIDED
render_mode cull_disabled;
#else
render_mode cull_back;
#endif
render_mode unshaded, fog_disabled, depth_draw_never, blend_mix;

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
