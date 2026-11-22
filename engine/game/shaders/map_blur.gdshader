/* The world map's depth blur (the game's ISWorldMap image space shader; its arithmetic from Community Shaders'
 * ISWorldMap.hlsl, a public rewrite): a 7x7 box of samples spaced
 *   min(max_blur, blur_scale * |depth - focal_depth|) texels, times near_scale nearer than the focal depth,
 * with the exe's defaults fWorldMapFocalDepth 45000, fWorldMapMaximumDepthBlur 0.45, fWorldMapDepthBlurScale 0.3 and
 * fWorldMapNearDepthBlurScale 4. Which setting fills which shader constant is NOT known (taken in that order), nor
 * the depth's units (taken as game units here). Drawn on a quad over the whole view before the transparent
 * geometry (render_priority), so the cloud bank stays sharp over it.
 */
shader_type spatial;
render_mode unshaded, depth_test_disabled, depth_draw_never, cull_disabled, skip_vertex_transform, fog_disabled;

uniform sampler2D screen_tex : hint_screen_texture, filter_linear;
uniform sampler2D depth_tex : hint_depth_texture, filter_nearest;
uniform float focal_depth = 642.6;  // metres (45000 units)
uniform float max_blur = 0.45;
uniform float blur_scale = 0.3;
uniform float near_scale = 4.0;
uniform float metres_per_unit = 0.0142875;

void vertex() {
	POSITION = vec4(VERTEX.xy, 1.0, 1.0);
}

void fragment() {
	float depth = texture(depth_tex, SCREEN_UV).r;
	vec4 view = INV_PROJECTION_MATRIX * vec4(SCREEN_UV * 2.0 - 1.0, depth, 1.0);
	float linear_depth = -view.z / view.w;
	float units = (linear_depth - focal_depth) / metres_per_unit;
	float spread = min(max_blur, blur_scale * abs(units));
	if (linear_depth < focal_depth) {
		spread *= near_scale;
	}
	vec2 texel = 1.0 / vec2(textureSize(screen_tex, 0));
	vec2 start = SCREEN_UV - 3.0 * texel * spread;
	vec3 sum = vec3(0.0);
	for (int i = 0; i < 7; i++) {
		for (int j = 0; j < 7; j++) {
			sum += texture(screen_tex, start + texel * spread * vec2(float(i), float(j))).rgb;
		}
	}
	ALBEDO = sum / 49.0;
}
