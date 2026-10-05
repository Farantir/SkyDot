/* Rain and snow (see render/weather.hpp).
 *
 * Camera-facing (rain turns about the vertical only), a random frame of the
 * atlas, the texture's alpha raised by `alpha_gain` (the rain texture's peaks
 * at 0.37).
 *
 * SkydotWeather::configure_precipitation() in render/weather.cpp uses this as it is.
 *
 * This comment is not part of the shader code: the loader cuts it off (README.md).
 */

shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_disabled, shadows_disabled;
uniform sampler2D tex : filter_linear_mipmap;
uniform vec4 tint = vec4(1.0);
uniform float alpha_gain = 1.0;
uniform int frames_h = 1;
uniform int frames_v = 1;
uniform bool upright = false;
void vertex() {
	mat4 world = MODEL_MATRIX;
	vec3 scale = vec3(length(world[0].xyz), length(world[1].xyz), length(world[2].xyz));
	if (upright) {
		vec3 side = normalize(cross(vec3(0.0, 1.0, 0.0), INV_VIEW_MATRIX[2].xyz));
		vec3 back = cross(side, vec3(0.0, 1.0, 0.0));
		world = mat4(vec4(side * scale.x, 0.0), vec4(0.0, scale.y, 0.0, 0.0), vec4(back * scale.z, 0.0), world[3]);
	} else {
		float a = INSTANCE_CUSTOM.x;
		mat4 spin = mat4(vec4(cos(a), -sin(a), 0.0, 0.0), vec4(sin(a), cos(a), 0.0, 0.0),
		                 vec4(0.0, 0.0, 1.0, 0.0), vec4(0.0, 0.0, 0.0, 1.0));
		world = mat4(vec4(normalize(INV_VIEW_MATRIX[0].xyz) * scale.x, 0.0),
		             vec4(normalize(INV_VIEW_MATRIX[1].xyz) * scale.y, 0.0),
		             vec4(normalize(INV_VIEW_MATRIX[2].xyz) * scale.z, 0.0), world[3]) * spin;
	}
	MODELVIEW_MATRIX = VIEW_MATRIX * world;
	float h = float(max(frames_h, 1));
	float v = float(max(frames_v, 1));
	float frame = min(floor(INSTANCE_CUSTOM.z * h * v), h * v - 1.0);
	UV = UV / vec2(h, v) + vec2(mod(frame, h) / h, floor(frame / h) / v);
}
void fragment() {
	vec4 c = texture(tex, UV);
	ALBEDO = tint.rgb * c.rgb;
	ALPHA = clamp(c.a * alpha_gain * tint.a * COLOR.a, 0.0, 1.0);
}
