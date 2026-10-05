/* Tree LOD shader (see render/lod.hpp).
 *
 * Trees: upright billboards turned to the camera. The instance transform
 * carries the base (origin) and the size (basis lengths); INSTANCE_CUSTOM the
 * atlas rectangle.
 *
 * lod_code() in render/lod.cpp builds the shader from
 *     shader_type spatial;
 *     render_mode diffuse_lambert, cull_disabled, depth_draw_opaque;
 * then lod_mask.gdshaderinc, then this file; then with_game_fog() inserts
 * game_fog.gdshaderinc after the render_mode line and ends fragment() with a FOG
 * line, and with_game_ambient() adds ambient_light_disabled to the render modes and
 * an EMISSION line at the end of fragment().
 *
 * This comment is not part of the shader code: the loader cuts it off (README.md).
 */

uniform sampler2D atlas : filter_linear_mipmap, repeat_disable;

void vertex() {
	vec3 origin = MODEL_MATRIX[3].xyz;
	float w = length(MODEL_MATRIX[0].xyz);
	float h = length(MODEL_MATRIX[1].xyz);
	vec3 to_camera = CAMERA_POSITION_WORLD - origin;
	to_camera.y = 0.0;
	vec3 facing = length(to_camera) > 0.001 ? normalize(to_camera) : vec3(0.0, 0.0, 1.0);
	vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), facing));
	vec3 world = origin + right * VERTEX.x * w + vec3(0.0, VERTEX.y * h, 0.0);
	skydot_world = world;
	POSITION = PROJECTION_MATRIX * (VIEW_MATRIX * vec4(world, 1.0));
	NORMAL = (VIEW_MATRIX * vec4(facing, 0.0)).xyz;
	UV = mix(INSTANCE_CUSTOM.xy, INSTANCE_CUSTOM.zw, UV);
}

void fragment() {
	if (skydot_masked(skydot_world)) {
		discard;
	}
	vec4 c = texture(atlas, UV);
	if (c.a < 0.5) {
		discard;
	}
	ALBEDO = c.rgb;
	ROUGHNESS = 1.0;
	SPECULAR = 0.0;
}
