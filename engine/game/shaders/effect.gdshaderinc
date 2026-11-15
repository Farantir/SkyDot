/* The effect shader: BSEffectShaderProperty (see render/materials.hpp).
 *
 * Not a whole shader: effect_code() in render/materials.cpp makes a stub for each variant,
 *     shader_type spatial;
 *     [#define SKYDOT_DOUBLE_SIDED] [#define SKYDOT_PARTICLES] [#define SKYDOT_LIT]
 *     [#define SKYDOT_ALPHA_ADD | #define SKYDOT_ALPHA_MUL | #define SKYDOT_ALPHA_BLEND | #define SKYDOT_ALPHA_TEST]
 *     #include "res://shaders/effect.gdshaderinc"
 * and the render modes below follow from the defines. The shader writes FOG itself;
 * SKYDOT_LIT effects take one light colour, not Godot's lighting (see light() below).
 */
#ifdef SKYDOT_DOUBLE_SIDED
render_mode cull_disabled;
#else
render_mode cull_back;
#endif
#ifdef SKYDOT_LIT
render_mode specular_disabled, ambient_light_disabled;
#else
render_mode unshaded;
#endif
#ifdef SKYDOT_ALPHA_TEST
render_mode alpha_to_coverage;
#endif
#if defined(SKYDOT_ALPHA_ADD)
// Additive as premultiplied alpha with ALPHA 0: the same colour as blend_add, and the target's alpha stays as it
// was, as in the game (an additive layer covers nothing: the skills screen's sky lets the world show through).
render_mode depth_draw_never, blend_premul_alpha;
#elif defined(SKYDOT_ALPHA_MUL)
render_mode depth_draw_never, blend_mul, fog_disabled;
#elif defined(SKYDOT_ALPHA_BLEND)
render_mode depth_draw_never, blend_mix;
#endif

#include "res://shaders/game_fog.gdshaderinc"

// Skyrim computes and blends effects in gamma space, and so does the scene
// here (see materials.hpp); additive output is premultiplied, as the game's.
uniform sampler2D source_tex : filter_linear_mipmap, repeat_enable;
uniform sampler2D palette_tex : filter_linear, repeat_disable;
uniform sampler2D depth_tex : hint_depth_texture;
uniform vec4 emission_color = vec4(1.0);
uniform float emission_strength = 1.0;
uniform vec4 falloff_params = vec4(1.0, 1.0, 0.0, 0.0);
uniform float soft_depth = 0.14;
uniform vec2 uv_scale = vec2(1.0);
uniform vec2 uv_offset = vec2(0.0);
uniform float alpha_cutoff = 0.5;
uniform bool use_vertex_colors = false;
uniform bool use_vertex_alpha = false;
uniform bool use_falloff = false;
uniform bool palette_color = false;
uniform bool palette_alpha = false;
uniform bool soft_effect = false;

#ifdef SKYDOT_PARTICLES
// Camera-facing quads sized by the particle transform's scale and turned by
// INSTANCE_CUSTOM.x; INSTANCE_CUSTOM.z picks a sub-texture rectangle
// (u, width, v, height).
uniform vec4 subtex_rects[64];
uniform int subtex_count = 0;
varying flat vec4 subtex_rect;

void vertex() {
	mat4 world = mat4(normalize(INV_VIEW_MATRIX[0]), normalize(INV_VIEW_MATRIX[1]),
			normalize(INV_VIEW_MATRIX[2]), MODEL_MATRIX[3]);
	float angle = INSTANCE_CUSTOM.x;
	world = world * mat4(vec4(cos(angle), -sin(angle), 0.0, 0.0),
			vec4(sin(angle), cos(angle), 0.0, 0.0), vec4(0.0, 0.0, 1.0, 0.0), vec4(0.0, 0.0, 0.0, 1.0));
	float size = length(MODEL_MATRIX[0].xyz);
	MODELVIEW_MATRIX = VIEW_MATRIX * world * mat4(vec4(size, 0.0, 0.0, 0.0),
			vec4(0.0, size, 0.0, 0.0), vec4(0.0, 0.0, size, 0.0), vec4(0.0, 0.0, 0.0, 1.0));
	MODELVIEW_NORMAL_MATRIX = mat3(MODELVIEW_MATRIX);
	if (subtex_count > 0) {
		vec4 r = subtex_rects[clamp(int(INSTANCE_CUSTOM.z), 0, subtex_count - 1)];
		UV = vec2(r.x + UV.x * r.y, r.z + UV.y * r.w);
		subtex_rect = r;
	} else {
		subtex_rect = vec4(0.0, 1.0, 0.0, 1.0);
	}
}
#endif

#ifdef SKYDOT_LIT
// Effect_Lighting: the game moves the colour from unlit towards colour x one
// light colour by the material's lighting influence, lerp(c, c * L, i), where
// L is DLightColor plus point light colours, with no N.L (Community Shaders,
// Effect.hlsl). DLightColor is not known; 1.1 x the weather's sunlight as it
// is (no image-space scale, the moonlight at night) fits the five exterior
// references without volumetric lighting (game-refs-2026-10-09/fxnovl_*; 1.5
// fitted the waterfalls with it, game-refs-2026-10-07, whose haze it took
// in). Adding the ambient does not: it fixes one waterfall's colour and breaks
// the other's. Interiors have none (place.gd).
// The point light is the reference's own emittance, for shapes flagged
// External_Emittance (XEMI naming a LIGH: its colour x fade; naming a REGN:
// its weather's Effect Lighting colour, SkydotEmittance; effect_emittance),
// not the lights around it: in Bleak Falls Barrow
// (game-refs-2026-10-19/s31_212820*) disabling the torch 190 units from the
// mist FXFog01 changes the mist by 4/1/0 in the frame's centre, where the mist
// adds 28/19/11 in the hue of its XEMI light (FXLightBleakDraugrTorch
// 190/131/65); SkyDot now 21/15/10 there (it was 41/31/22 with the scene's
// lights, which also lit the cobwebs the game hardly shows). SkyDot's mist
// still covers about five times the game's area (open).
// Inside, shapes without XEMI take the cell's directional colour (XCLL) as
// DLightColor: with none, Bleak Falls Barrow's lit spider webs (lighting
// influence 1, alpha blended) were black and its additive light beams
// (FXAmbBeamDust) were gone. The mist (XEMI) keeps none (skydot_effect_light_xemi
// zero inside): with the directional colour it came out greyer than the
// game's. Point lights add to the shapes without emittance only (light()):
// the game's mist does not follow the torch next to it (above), but its
// spider webs take the hue of the cyan lights around them (0x2EDF0,
// 0.71/0.97/0.99, fade 5): Bleak Falls Barrow's web close-up went from 22 %
// to 4 % under the game's luma and the spider room from 25 % to 1 %
// (skycompare 2026-11-07, shots 133737 and 134202).
global uniform vec3 skydot_effect_light;
global uniform vec3 skydot_effect_light_xemi;
uniform float lighting_influence = 0.55;
uniform bool external_emittance = false;
instance uniform vec3 effect_emittance = vec3(0.0);
varying flat float point_light_share;
// Point lights add to DLightColor for shapes without emittance (Community
// Shaders, Effect.hlsl GetLightingColor: no N.L); the directional light is
// skydot_effect_light, in EMISSION.
void light() {
	if (!LIGHT_IS_DIRECTIONAL) {
		DIFFUSE_LIGHT += point_light_share * ATTENUATION * LIGHT_COLOR / PI;
	}
}
#endif


// Follows the game's effect pixel shader as reverse-engineered by Community
// Shaders (Effect.hlsl); NifSkope's version leaves the texture alpha out of the
// palette row.
void fragment() {
	vec2 source_uv = UV * uv_scale + uv_offset;
	vec4 source = texture(source_tex, source_uv);
#ifdef SKYDOT_PARTICLES
	// A sub-texture is one cell of an atlas, and the game's own small mips
	// blur the cells into each other: from 16 texels a cell down, its
	// transparent border fills in and far particles turn into squares.
	vec2 cell = subtex_rect.yw * vec2(textureSize(source_tex, 0));
	float max_lod = max(log2(max(min(cell.x, cell.y), 1.0)) - 4.0, 0.0);
	source = textureLod(source_tex, source_uv, min(textureQueryLod(source_tex, source_uv).y, max_lod));
#endif
	vec4 vertex = vec4(1.0);
	if (use_vertex_colors) {
		vertex.rgb = COLOR.rgb;
	}
	if (use_vertex_alpha) {
		vertex.a = COLOR.a;
	}
	float falloff = 1.0;
	if (use_falloff) {
		// x/y: start/stop angle (cosines), z/w: start/stop opacity.
		falloff = smoothstep(falloff_params.y, falloff_params.x, abs(dot(NORMAL, VIEW)));
		falloff = mix(max(falloff_params.w, 0.0), min(falloff_params.z, 1.0), falloff);
	}
	float alpha_mult = emission_color.a * emission_color.a;
	vec3 color = source.rgb * vertex.rgb * emission_color.rgb;
	// With greyscale to palette alpha the texture's alpha picks the palette
	// column only, not the row (Community Shaders, Effect.hlsl).
	float alpha = (palette_alpha ? 1.0 : source.a) * vertex.a * falloff * alpha_mult;
	if (soft_effect) {
		// Fade where the surface nears the geometry behind it.
		float depth = texture(depth_tex, SCREEN_UV).r;
		vec4 behind = INV_PROJECTION_MATRIX * vec4(SCREEN_UV * 2.0 - 1.0, depth, 1.0);
		alpha *= clamp((VERTEX.z - behind.z / behind.w) / soft_depth, 0.0, 1.0);
	}
	if (palette_color) {
		color = texture(palette_tex, vec2(source.g,
				clamp(vertex.r * emission_color.r, 0.0, 1.0))).rgb;
	}
	if (palette_alpha) {
		// The row is the alpha so far, without the texture's: with it, the
		// waterfall foam (fxwaterfallbodyslope) fell to the gradient's clear
		// rows in patches and the sheet under it showed twice the game's
		// contrast.
		alpha = texture(palette_tex, vec2(source.a, clamp(alpha, 0.0, 1.0))).a;
	}
	color *= emission_strength;
#ifdef SKYDOT_ALPHA_ADD
	ALBEDO = max(color * alpha, vec3(0.0));
	ALPHA = 0.0;
	// Fog would mix the transparent (black) parts towards its colour, and
	// adding that draws every quad as a square far away. Fade towards black
	// by the fog's amount instead.
	FOG = vec4(0.0, 0.0, 0.0, skydot_game_fog(VERTEX).a);
#elif defined(SKYDOT_ALPHA_MUL)
	// The framebuffer times the colour (contact shadows under clutter);
	// alpha takes no part. Fog fades it towards white, which changes nothing.
	ALBEDO = mix(max(color, vec3(0.0)), vec3(1.0), skydot_game_fog(VERTEX).a);
#else
	ALBEDO = max(color, vec3(0.0));
	FOG = skydot_game_fog(VERTEX);
#endif
#ifdef SKYDOT_LIT
	ROUGHNESS = 1.0;
	SPECULAR = 0.0;
#endif
#ifdef SKYDOT_ALPHA_TEST
	ALPHA = clamp((alpha - alpha_cutoff) / max(fwidth(alpha), 0.0001) + 0.5, 0.0, 1.0);
	ALPHA_SCISSOR_THRESHOLD = 0.01;
#endif
#ifdef SKYDOT_ALPHA_BLEND
	ALPHA = alpha;
#endif
#ifdef SKYDOT_LIT
	// The flag alone is set on most effect shapes (the webs too); only a
	// reference with XEMI gives a colour.
	bool emitting = external_emittance && any(greaterThan(effect_emittance, vec3(0.0)));
	point_light_share = emitting ? 0.0 : lighting_influence;
	EMISSION += ALBEDO * ((1.0 - lighting_influence) + lighting_influence * (emitting ? 1.1 * skydot_effect_light_xemi + effect_emittance : 1.1 * skydot_effect_light));
#endif
}
