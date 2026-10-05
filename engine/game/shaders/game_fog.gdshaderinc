/* The game's fog and directional ambient, as globals and functions.
 *
 * with_game_fog() in render/materials.cpp inserts this after the render_mode line
 * of every shader that has fog (materials, terrain, water, LOD) and, unless the
 * shader writes FOG itself, ends fragment() with
 *     FOG = skydot_game_fog(VERTEX);
 * with_game_ambient() adds, for lit shaders, ambient_light_disabled to the render
 * modes and, at the end of fragment(),
 *     EMISSION += ALBEDO * skydot_ambient((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
 * The ambient part is here, not in its own file, because both are inserted
 * together and in this order.
 * SkydotMaterials::sync_fog() sets the global uniforms.
 *
 * This comment is not part of the shader code: the loader cuts it off (README.md).
 */

global uniform vec4 skydot_fog; // near, far (metres), power, max
global uniform vec3 skydot_fog_near_color; // the game's (gamma) colours
global uniform vec3 skydot_fog_far_color;
// The game's directional ambient (DALC) as its shaders get it: per colour
// channel a linear function of the world normal (Godot's axes) plus a
// constant, so mul(DirectionalAmbient, float4(normal, 1)).
global uniform vec4 skydot_ambient_r;
global uniform vec4 skydot_ambient_g;
global uniform vec4 skydot_ambient_b;
vec3 skydot_ambient(vec3 world_normal) {
	vec4 n = vec4(world_normal, 1.0);
	return max(vec3(dot(skydot_ambient_r, n), dot(skydot_ambient_g, n), dot(skydot_ambient_b, n)), vec3(0.0));
}
vec4 skydot_game_fog(vec3 view_vertex) {
	float far = max(skydot_fog.y, skydot_fog.x + 0.001);
	float ramp = pow(clamp((length(view_vertex) - skydot_fog.x) / (far - skydot_fog.x), 0.0, 1.0),
			max(skydot_fog.z, 0.001));
	return vec4(mix(skydot_fog_near_color, skydot_fog_far_color, ramp), min(ramp, skydot_fog.w));
}
