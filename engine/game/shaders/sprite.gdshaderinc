/* Stars, the sun and its glare add light; the moons cover what is behind.
 *
 * Every shape is in its texture's alpha: the constellation textures' colour
 * is a nebula under transparent texels, only the stars are opaque.
 *
 * The body of sprite_add.gdshader (stars, sun, glare) and sprite_mix.gdshader (moons),
 * which differ in their blend mode. SkydotWeather::build_sky_objects() in render/weather.cpp
 * makes their materials.
 */
uniform sampler2D tex : filter_linear_mipmap;
uniform vec4 tint = vec4(1.0);
uniform bool use_alpha = true;
#include "res://shaders/far_plane.gdshaderinc"
void fragment() {
	vec4 c = texture(tex, UV);
	ALBEDO = c.rgb * tint.rgb;
	ALPHA = clamp((use_alpha ? c.a : 1.0) * tint.a * COLOR.a, 0.0, 1.0);
}
