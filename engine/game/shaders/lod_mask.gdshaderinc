/* LOD shaders: discard over cells built at full detail (see render/lod.hpp).
 *
 * Included by every LOD shader (lod_terrain, lod_object, lod_water, lod_tree .gdshader) after
 * game_fog.gdshaderinc. It declares skydot_world, which their vertex() sets.
 * SkydotLod::apply_mask() sets the uniforms. The mask has a second state, 0.5, where large
 * references are drawn (render/large_refs.hpp): there only the object shapes made of them hide
 * (lod_object's `large_ref_shape`).
 */

uniform sampler2D skydot_cell_mask : filter_nearest, repeat_disable;
uniform vec2 skydot_mask_origin;
uniform float skydot_mask_size = 1.0;
uniform float skydot_unit_scale;
uniform float skydot_cell_units;
varying vec3 skydot_world;

/* The mask of the cell under `world`: 0 nothing built, 0.5 the cell's large references are
 * drawn as models, 1 the cell is built at full detail. */
float skydot_mask_state(vec3 world) {
	vec2 game = vec2(world.x, -world.z) / skydot_unit_scale;
	vec2 cell = floor(game / skydot_cell_units) - skydot_mask_origin;
	if (any(lessThan(cell, vec2(0.0))) || any(greaterThanEqual(cell, vec2(skydot_mask_size)))) {
		return 0.0;
	}
	return texture(skydot_cell_mask, (cell + 0.5) / skydot_mask_size).r;
}

/* Everything hides over a cell built at full detail. */
bool skydot_masked(vec3 world) {
	return skydot_mask_state(world) > 0.75;
}
