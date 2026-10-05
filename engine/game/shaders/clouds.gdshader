/* Cloud layers: two weathers' textures for one layer, cross-faded by `mix_t`.
 *
 * make_shader() in render/weather.cpp replaces the line %FAR% with far_plane.gdshaderinc.
 * SkydotWeather::build_clouds() makes one material of it per cloud layer.
 *
 * This comment is not part of the shader code: the loader cuts it off (README.md).
 */

shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_disabled, fog_disabled, shadows_disabled;
uniform sampler2D tex_a : filter_linear_mipmap, repeat_enable;
uniform sampler2D tex_b : filter_linear_mipmap, repeat_enable;
uniform vec4 color_a = vec4(0.0);
uniform vec4 color_b = vec4(0.0);
uniform vec2 offset_a;
uniform vec2 offset_b;
uniform float mix_t = 1.0;
uniform vec3 flash_color;
uniform float flash = 0.0;
%FAR%
void fragment() {
	vec4 a = texture(tex_a, UV + offset_a) * color_a;
	vec4 b = texture(tex_b, UV + offset_b) * color_b;
	float wa = a.a * (1.0 - mix_t);
	float wb = b.a * mix_t;
	float alpha = wa + wb;
	vec3 rgb = alpha > 0.0001 ? (a.rgb * wa + b.rgb * wb) / alpha : vec3(0.0);
	// The dome's vertex colours are (1, 0, 0, fade): only the alpha means
	// anything.
	ALBEDO = rgb + flash_color * flash * 0.5;
	ALPHA = clamp(alpha * COLOR.a, 0.0, 1.0);
}
