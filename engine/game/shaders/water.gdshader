/* The water shader: the game's water (see render/water.hpp).
 *
 * A whole shader that WaterMaterials::shader() loads as it is. Unlit, with the game's fog
 * (game_fog.gdshaderinc).
 */
shader_type spatial;
render_mode unshaded, cull_disabled, blend_mix, depth_draw_always;

#include "res://shaders/game_fog.gdshaderinc"

// The game's water (Community Shaders, Water.hlsl, the above-water path near
// the camera), in the scene's gamma space. What lies below, refracted, gives
// way to the shallow-to-deep colour as the view ray's path through the water
// nears the fog's far plane; the reflection (the reflection colour mixed with
// the sky, or the scene where a short screen-space march finds it, for the
// game's cubemap and SSR) is laid over it by Fresnel; the sun adds its
// specular highlight and the sparkle (small facets that face a fixed
// direction). The depth controls scale reflection, refraction, normals and
// sun where the water is shallow.
uniform vec3 shallow_color = vec3(0.15, 0.2, 0.15);
uniform vec3 deep_color = vec3(0.02, 0.06, 0.07);
uniform vec3 reflection_color = vec3(0.45, 0.52, 0.6);
uniform float fresnel_amount = 0.05;
uniform float reflectivity = 1.0;
uniform float sun_specular_power = 1021.0;
uniform float sun_specular_magnitude = 4.65;
uniform float sun_sparkle_power = 3521.0;
uniform float sun_sparkle_magnitude = 2.89;
// Metres; the fog amount is the exponent of the fog's curve.
uniform float fog_near = -0.143;
uniform float fog_far = 1.6;
uniform float fog_amount = 0.7;
// Metres the refraction moves the view below, per unit of normal.
uniform float refraction_magnitude = 0.571;
// How much of the normal the reflection follows.
uniform float reflection_magnitude = 0.42;
// Reflections, refraction, normals, sun: how much of each is left where the
// water is shallow is 1 - this.
uniform vec4 depth_control = vec4(0.3, 0.95, 0.25, 0.9);
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

// The sparkle's facet direction, the game's (-0.099, -0.099, 0.99) with
// Skyrim's north as Godot's -Z.
const vec3 SPARKLE_FACING = vec3(-0.099, 0.99, 0.099);
// The game's sun colour for water (SunColor x SunDir.w) is brighter than the
// one our lighting uses: fitted on 194857's pre-grade game frame (the share
// of sparkle pixels above 0.3..0.8 matches at 9).
const float SUN_FIT = 9.0;
// How much of the reflected scene shows (the game's WaterParams.w on its
// cubemap, and its SSR fraction): fitted on 194857 against the graded game
// frame (grid error 8.0 at 1.0 on the fallbacks only, 6.9 at 0.3 on all).
const float CUBEMAP_FIT = 0.3;

varying vec3 world_pos;

void vertex() {
	world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

// One layer's normal map sample, in the map's frame (z up).
vec3 layer_sample(vec4 layer) {
	// Skyrim's north is Godot's -Z.
	vec2 wind = vec2(sin(layer.y), -cos(layer.y)) * layer.z * TIME;
	return texture(noise_tex, world_pos.xz / max(layer.x, 0.01) + wind).xyz * 2.0;
}

vec3 sky(vec3 direction) {
	float y = max(direction.y, 0.0);
	return mix(skydot_sky_horizon, skydot_sky_upper, clamp(y * y, 0.0, 1.0));
}

// The scene's view-space position under a screen point.
vec3 scene_at(vec2 uv, mat4 inv_projection) {
	float d = texture(depth_tex, uv).r;
	vec4 p = inv_projection * vec4(uv * 2.0 - 1.0, d, 1.0);
	return p.xyz / p.w;
}

void fragment() {
	vec3 up_view = (VIEW_MATRIX * vec4(0.0, 1.0, 0.0, 0.0)).xyz;

	// The view ray's path through the water and the depth below the surface,
	// over the fog's far plane (the game's distanceMul .x/.y and .z/.w).
	vec3 behind = scene_at(SCREEN_UV, INV_PROJECTION_MATRIX);
	vec3 through = VERTEX - behind;
	float path = (behind.z < VERTEX.z) ? length(through) : 0.0;
	float below = max(dot(through, up_view), 0.0);
	float far = max(fog_far, 0.01);
	vec2 distance_mul = clamp(vec2(path, below) / far, 0.0, 1.0);
	vec4 depth = depth_control * (distance_mul.xxyy - 1.0) + 1.0;

	// Normal (Water.hlsl GetWaterNormal): the first layer's map counts with
	// its z less 2, the others whole, over an up vector.
	vec3 n = vec3(0.0, 0.0, 1.0) + layer_0.w * (layer_sample(layer_0) + vec3(-1.0, -1.0, -2.0))
			+ layer_1.w * (layer_sample(layer_1) - 1.0) + layer_2.w * (layer_sample(layer_2) - 1.0);
	n = normalize(mix(vec3(0.0, 0.0, 1.0), normalize(n), depth.z));
	vec3 world_normal = normalize(vec3(n.x, n.z, n.y));
	vec3 normal = normalize((VIEW_MATRIX * vec4(world_normal, 0.0)).xyz);

	vec3 view_dir = -VIEW; // from the eye
	vec3 world_view = normalize((INV_VIEW_MATRIX * vec4(view_dir, 0.0)).xyz);
	float fresnel = fresnel_amount + (1.0 - fresnel_amount) * pow(1.0 - clamp(dot(normal, VIEW), 0.0, 1.0), 5.0);

	// Refraction: the view below moves with the normal; where the moved point
	// lies above the water the unmoved one is used.
	vec3 shift = (VIEW_MATRIX * vec4(world_normal.x, 0.0, world_normal.z, 0.0)).xyz
			* refraction_magnitude * depth.y;
	vec4 shifted = PROJECTION_MATRIX * vec4(VERTEX + shift, 1.0);
	vec2 refracted_uv = clamp(shifted.xy / shifted.w * 0.5 + 0.5, vec2(0.0), vec2(1.0));
	if (scene_at(refracted_uv, INV_PROJECTION_MATRIX).z > VERTEX.z) {
		refracted_uv = SCREEN_UV;
	}
	vec3 refracted = textureLod(screen_tex, refracted_uv, 0.0).rgb;
	vec3 water = mix(shallow_color, deep_color, distance_mul.y);
	float fog_range = max(fog_far - fog_near, 0.01);
	float fogged = 1.0 - pow(clamp((far - distance_mul.x * far) / fog_range, 0.0, 1.0), max(fog_amount, 0.0001));
	vec3 diffuse = mix(refracted, water, fogged);

	// Reflection along a normal flattened by the reflection magnitude: the
	// sky, or the scene where a short march through the depth buffer hits it.
	vec3 reflect_normal = normalize(world_normal * reflection_magnitude + vec3(0.0, 1.0 - reflection_magnitude, 0.0));
	vec3 world_reflected = reflect(world_view, reflect_normal);
	vec3 reflected = (VIEW_MATRIX * vec4(world_reflected, 0.0)).xyz;
	vec3 environment = sky(world_reflected) * CUBEMAP_FIT;
	vec3 p = VERTEX;
	float step_length = max(0.2, length(VERTEX) * 0.02);
	for (int i = 0; i < 24; ++i) {
		p += reflected * step_length;
		vec4 clip = PROJECTION_MATRIX * vec4(p, 1.0);
		vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
		if (uv.y < 0.0 && uv.x >= 0.0 && uv.x <= 1.0 && p.z <= -0.05) {
			// Left through the top: what the frame shows along its top edge
			// above stands in for the game's cubemap (a gorge's cliff, else
			// the sky), blurred.
			environment = textureLod(screen_tex, vec2(uv.x, 0.0), 4.0).rgb * CUBEMAP_FIT;
			break;
		}
		if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || p.z > -0.05) {
			break;
		}
		float scene_z = scene_at(uv, INV_PROJECTION_MATRIX).z;
		if (scene_z > p.z && scene_z - p.z < step_length * 2.0) {
			float edge = smoothstep(0.0, 0.1, min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)));
			environment = mix(environment, textureLod(screen_tex, uv, 0.0).rgb * CUBEMAP_FIT, edge);
			break;
		}
		step_length *= 1.15;
	}
	vec3 specular = mix(reflection_color, environment, reflectivity);

	// The sun (GetSunColor): its highlight in the reflected direction and the
	// sparkle.
	vec3 sun_reflected = reflect(world_view, world_normal);
	float highlight = pow(clamp(dot(sun_reflected, skydot_sun_direction), 0.0, 1.0), max(sun_specular_power, 1.0));
	float sparkle = pow(clamp(dot(world_normal, SPARKLE_FACING), 0.0, 1.0), max(sun_sparkle_power, 1.0));
	vec3 sun = skydot_sun_color * SUN_FIT * (0.3 * sun_specular_magnitude * highlight + 0.1 * sun_sparkle_magnitude * sparkle);

	ALBEDO = mix(diffuse, specular, fresnel * depth.x) + sun * depth.w;
	// Fade in over the first 10 cm, so shorelines do not show a hard edge.
	ALPHA = clamp((behind.z < VERTEX.z ? VERTEX.z - behind.z : 0.0) / 0.1, 0.0, 1.0);
	FOG = skydot_game_fog(VERTEX);
}
