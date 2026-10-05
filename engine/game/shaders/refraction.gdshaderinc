/* The refraction shader: lighting shapes with SLSF1_REFRACTION (see render/materials.hpp).
 *
 * refraction_code() in render/materials.cpp puts this after
 *     shader_type spatial;
 *     render_mode cull_back | cull_disabled, unshaded, fog_disabled, depth_draw_never, blend_mix;
 * Nothing else is added: the screen texture is already fogged.
 *
 * This comment is not part of the shader code: the loader cuts it off (README.md).
 */

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
