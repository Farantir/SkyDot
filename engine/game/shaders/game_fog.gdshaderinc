/* The game's fog and directional ambient, as globals and functions.
 *
 * skydot_game_fog(VERTEX) is Skyrim's fog: min(max, ((distance - near) / (far - near))^power),
 * coloured from the near to the far fog colour by the same ramp. Godot's depth fog differs
 * (smoothstep, one colour).
 *
 * Included by every shader that has them (lighting, effect, terrain, water, LOD): its fragment()
 * writes FOG = skydot_game_fog(VERTEX); (effect, which fades by blend mode, writes its own) and,
 * if it is lit, ends with
 *     EMISSION += ALBEDO * skydot_ambient((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
 * with ambient_light_disabled in its render modes, so the game's ambient replaces Godot's.
 * The ambient is here, not in its own file, because they always come together.
 * SkydotMaterials::sync_fog() sets the global uniforms, and SkydotMaterials::ensure_fog_globals()
 * must have registered them before any of these shaders compiles.
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
