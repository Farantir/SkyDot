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
// The cell's flowmap (textures/water/<plugin>/flow.X.Y.dds, 64 x 64 per cell,
// north row first): rg the flow's rotation (sin, cos), b the tiling term, a
// the scroll; and the water's flow normals (WATR NAM4), tiled along it.
uniform bool flow_enabled = false;
uniform sampler2D flow_tex : filter_nearest, repeat_disable;
uniform sampler2D flow_normals_tex : filter_linear_mipmap, repeat_enable;
// The cell's grid position, x east and y south (cells), for flow UVs that run
// on across cell borders.
uniform vec2 flow_cell = vec2(0.0);
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
// The game's framebuffer range (WaterParams.w), which scales both the
// reflected scene (cubemap, and here the SSR too) and the refracted view
// below: fitted on 194857 against the graded game frame (grid error 8.0 at
// 1.0 on the reflection fallbacks only, 6.9 at 0.3 on all reflections, 5.5
// with the refraction as well; 194938's pool floor goes dark as in the game).
const float FRAMEBUFFER_RANGE_FIT = 0.3;

// A cell's side in metres (4096 units).
const float CELL_SIDE = 58.5216;
// The game's flowmap scroll timer (ReflectionColor.w) per second: game
// seconds at the default timescale, 20. Measured on the Riverwood river
// (game-refs-2026-10-13 rw_open_seq): the pattern's screen motion fits 15..23.
const float FLOW_TIME_SCALE = 20.0;

varying vec3 world_pos;
// The point's place in its cell, x east and y south, 0..1 (the cell water
// plane's own coordinates; the game's TexCoord0).
varying vec2 cell_uv;

void vertex() {
	world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	cell_uv = VERTEX.xz / CELL_SIDE + 0.5;
}

// One flowmap sample's normal (Water.hlsl GetFlowmapNormal): the flow normals
// tiled 64 times per cell, scaled by the b term, rotated by the texel's flow
// and scrolled along it; xy the map's normal, z the b term.
vec3 flowmap_normal(vec2 shift, float multiplier, float offset) {
	vec4 flow = textureLod(flow_tex, cell_uv + shift, 0.0);
	vec2 v = 64.0 * (flow_cell + cell_uv) * sqrt(1.01 - flow.z);
	vec2 sc = flow.xy * 2.0 - 1.0;
	// transpose(float2x2(sc.x, sc.y, -sc.y, sc.x)) * v
	v = vec2(sc.x * v.x - sc.y * v.y, sc.y * v.x + sc.x * v.y);
	vec2 scroll = vec2(multiplier * 0.001 * TIME * FLOW_TIME_SCALE * flow.w, 0.0);
	vec2 uv = offset + v - scroll;
	vec2 dx = dFdx(uv);
	vec2 dy = dFdy(uv);
	float mip = clamp(0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-12)), 0.0, 5.0);
	vec2 scaled = offset + v * exp2(-mip) - scroll;
	return vec3(textureLod(flow_normals_tex, scaled, mip).xy, flow.z);
}

// The flowmap's normal (Water.hlsl GetWaterNormal, FLOWMAP): four samples,
// each half a flowmap texel apart, blended by where the point lies in its
// texel; in the map's frame (z up), y south as layer_sample's.
vec3 flow_normal(float depth_normals) {
	vec2 shift = vec2(1.0 / 128.0);
	vec2 mul = 1.0 - abs(fract(cell_uv * 64.0) * 2.0 - 1.0);
	vec3 n0 = flowmap_normal(shift, 9.92, 0.0);
	vec3 n1 = flowmap_normal(vec2(0.0, shift.y), 10.64, 0.27);
	vec3 n2 = flowmap_normal(vec2(0.0), 8.0, 0.0);
	vec3 n3 = flowmap_normal(vec2(shift.x, 0.0), 8.48, 0.62);
	vec2 weighted = mul.y * (mul.x * n2.xy + (1.0 - mul.x) * n3.xy)
			+ (1.0 - mul.y) * (mul.x * n1.xy + (1.0 - mul.x) * n0.xy);
	vec2 denominator = sqrt(mul * mul + (1.0 - mul) * (1.0 - mul));
	vec2 xy = (weighted - 0.5) / (denominator.x * denominator.y) * max(0.4, depth_normals);
	// The game reads the map's y as north.
	return vec3(xy.x, -xy.y, sqrt(max(1.0 - dot(xy, xy), 0.0)));
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
	if (flow_enabled) {
		// Near the camera the flowmap's normal stands alone (distanceFactor 1).
		n = normalize(flow_normal(depth.z));
	}
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
	vec3 diffuse = mix(refracted * FRAMEBUFFER_RANGE_FIT, water, fogged);

	// Reflection along a normal flattened by the reflection magnitude: the
	// sky, or the scene where a short march through the depth buffer hits it.
	vec3 reflect_normal = normalize(world_normal * reflection_magnitude + vec3(0.0, 1.0 - reflection_magnitude, 0.0));
	vec3 world_reflected = reflect(world_view, reflect_normal);
	vec3 reflected = (VIEW_MATRIX * vec4(world_reflected, 0.0)).xyz;
	vec3 environment = sky(world_reflected) * FRAMEBUFFER_RANGE_FIT;
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
			environment = textureLod(screen_tex, vec2(uv.x, 0.0), 4.0).rgb * FRAMEBUFFER_RANGE_FIT;
			break;
		}
		if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || p.z > -0.05) {
			break;
		}
		float scene_z = scene_at(uv, INV_PROJECTION_MATRIX).z;
		if (scene_z > p.z && scene_z - p.z < step_length * 2.0) {
			float edge = smoothstep(0.0, 0.1, min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y)));
			environment = mix(environment, textureLod(screen_tex, uv, 0.0).rgb * FRAMEBUFFER_RANGE_FIT, edge);
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
