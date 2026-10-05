/* Stars, the sun and its glare add light; the moons cover what is behind.
 *
 * Every shape is in its texture's alpha: the constellation textures' colour
 * is a nebula under transparent texels, only the stars are opaque.
 *
 * SkydotWeather::build_sky_objects() in render/weather.cpp makes two shaders of
 * this: %BLEND% is blend_add (stars, sun, glare) or blend_mix (moons), and make_shader()
 * replaces the line %FAR% with far_plane.gdshaderinc.
 *
 * This comment is not part of the shader code: the loader cuts it off (README.md).
 */

shader_type spatial;
render_mode unshaded, %BLEND%, depth_draw_never, cull_disabled, fog_disabled, shadows_disabled;
uniform sampler2D tex : filter_linear_mipmap;
uniform vec4 tint = vec4(1.0);
uniform bool use_alpha = true;
%FAR%
void fragment() {
	vec4 c = texture(tex, UV);
	ALBEDO = c.rgb * tint.rgb;
	ALPHA = clamp((use_alpha ? c.a : 1.0) * tint.a * COLOR.a, 0.0, 1.0);
}
