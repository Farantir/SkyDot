/* The water shader: the game's water (see render/water.hpp).
 *
 * A whole shader that WaterMaterials::shader() loads as it is. Unlit, with the game's fog
 * (game_fog.gdshaderinc).
 */
shader_type spatial;
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never;

#include "res://shaders/game_fog.gdshaderinc"

// The game's water (Community Shaders, Water.hlsl), in the scene's gamma
// space: what lies below, refracted, gives way to the shallow-to-deep colour
// with depth; that gives way to the reflection by Fresnel; the sun adds a
// glint. The reflection is the sky's colour in the reflected direction, or
// the scene where a short screen-space march finds it (the game uses a sky
// cubemap and its own screen-space reflections).
uniform vec3 shallow_color = vec3(0.15, 0.2, 0.15);
uniform vec3 deep_color = vec3(0.02, 0.06, 0.07);
uniform vec3 reflection_color = vec3(0.45, 0.52, 0.6);
uniform float fresnel_amount = 0.05;
uniform float reflectivity = 1.0;
uniform float refraction = 0.02;
uniform float sun_specular_power = 50.0;
uniform float sun_sparkle = 1.0;
// Metres of water over which shallow turns deep.
uniform float fog_far = 1.6;
uniform sampler2D noise_tex : hint_normal, filter_linear_mipmap, repeat_enable;
// Per layer: metres per tile, direction (radians), speed (tiles per second),
// amplitude.
uniform vec4 layer_0 = vec4(27.0, 0.0, 0.02, 0.7);
uniform vec4 layer_1 = vec4(96.0, 0.0, 0.01, 0.6);
uniform vec4 layer_2 = vec4(7.0, 0.0, 0.1, 0.5);
uniform sampler2D depth_tex : hint_depth_texture;
uniform sampler2D screen_tex : hint_screen_texture, filter_linear_mipmap;
global uniform vec3 skydot_sun_direction; // towards the sun, world
global uniform vec3 skydot_sun_color;     // the game's, with the image space's scale; 0 inside
global uniform vec3 skydot_sky_upper;
global uniform vec3 skydot_sky_horizon;

varying vec3 world_pos;

void vertex() {
	world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

vec2 ripple(vec4 layer) {
	// Skyrim's north is Godot's -Z.
	vec2 wind = vec2(sin(layer.y), -cos(layer.y)) * layer.z * TIME;
	vec2 n = texture(noise_tex, world_pos.xz / max(layer.x, 0.01) + wind).xy * 2.0 - 1.0;
	return n * layer.w;
}

vec3 sky(vec3 direction) {
	float y = max(direction.y, 0.0);
	return mix(skydot_sky_horizon, skydot_sky_upper, clamp(y * y, 0.0, 1.0));
}

void fragment() {
	// A fine copy of the smallest layer gives the sun small facets to glint
	// on, as the game's water has.
	vec4 fine = vec4(layer_2.x * 0.15, layer_2.y + 0.7, layer_2.z * 2.0, layer_2.w * 0.6);
	vec2 n = ripple(layer_0) + ripple(layer_1) + ripple(layer_2) + ripple(fine);
	vec3 world_normal = normalize(vec3(n.x, 1.5, n.y));
	vec3 normal = normalize((VIEW_MATRIX * vec4(world_normal, 0.0)).xyz);

	// Water depth under this point along the view ray.
	float depth = texture(depth_tex, SCREEN_UV).r;
	vec4 behind = INV_PROJECTION_MATRIX * vec4(SCREEN_UV * 2.0 - 1.0, depth, 1.0);
	float thickness = max(VERTEX.z - behind.z / behind.w, 0.0);
	float deep = clamp(thickness / max(fog_far, 0.01), 0.0, 1.0);

	vec3 below = textureLod(screen_tex, SCREEN_UV + n * refraction * deep, 0.0).rgb;
	vec3 water = mix(shallow_color, deep_color, deep);
	vec3 diffuse = mix(below, water, deep);

	vec3 view_dir = -VIEW; // from the eye
	float fresnel = fresnel_amount + (1.0 - fresnel_amount) * pow(1.0 - clamp(dot(normal, VIEW), 0.0, 1.0), 5.0);

	// Reflection: march the reflected ray a little through the depth buffer.
	vec3 reflected = reflect(view_dir, normal);
	vec3 world_reflected = normalize((INV_VIEW_MATRIX * vec4(reflected, 0.0)).xyz);
	vec3 reflection = sky(world_reflected) * reflection_color / max(max(reflection_color.r, reflection_color.g), max(reflection_color.b, 0.001));
	vec3 p = VERTEX;
	float step_length = max(0.2, length(VERTEX) * 0.02);
	for (int i = 0; i < 24; ++i) {
		p += reflected * step_length;
		vec4 clip = PROJECTION_MATRIX * vec4(p, 1.0);
		vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
		if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || p.z > -0.05) {
			break;
		}
		float d = texture(depth_tex, uv).r;
		vec4 hit = INV_PROJECTION_MATRIX * vec4(uv * 2.0 - 1.0, d, 1.0);
		float scene_z = hit.z / hit.w;
		if (scene_z > p.z && scene_z - p.z < step_length * 2.0) {
			float edge = smoothstep(0.0, 0.1, min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)));
			reflection = mix(reflection, textureLod(screen_tex, uv, 0.0).rgb, edge);
			break;
		}
		step_length *= 1.15;
	}

	vec3 color = mix(diffuse, reflection, clamp(fresnel * reflectivity, 0.0, 1.0) * max(deep, 0.6));
	float glint = pow(clamp(dot(world_reflected, skydot_sun_direction), 0.0, 1.0), max(sun_specular_power, 1.0));
	color += glint * skydot_sun_color * sun_sparkle;

	ALBEDO = color;
	// Fade in over the first 10 cm, so shorelines do not show a hard edge.
	ALPHA = clamp(thickness / 0.1, 0.0, 1.0);
	FOG = skydot_game_fog(VERTEX);
}
