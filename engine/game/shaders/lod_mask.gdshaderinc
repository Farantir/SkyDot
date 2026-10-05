/* LOD shaders: discard over cells built at full detail (see render/lod.hpp).
 *
 * Shared by every LOD shader: lod_code() in render/lod.cpp puts this after the
 * render_mode line and before the shader's own file (lod_terrain, lod_object, lod_water,
 * lod_tree). It declares skydot_world, which their vertex() sets.
 * SkydotLod::apply_mask() sets the uniforms.
 *
 * This comment is not part of the shader code: the loader cuts it off (README.md).
 */

uniform sampler2D skydot_cell_mask : filter_nearest, repeat_disable;
uniform vec2 skydot_mask_origin;
uniform float skydot_mask_size = 1.0;
uniform float skydot_unit_scale;
uniform float skydot_cell_units;
varying vec3 skydot_world;

bool skydot_masked(vec3 world) {
	vec2 game = vec2(world.x, -world.z) / skydot_unit_scale;
	vec2 cell = floor(game / skydot_cell_units) - skydot_mask_origin;
	if (any(lessThan(cell, vec2(0.0))) || any(greaterThanEqual(cell, vec2(skydot_mask_size)))) {
		return false;
	}
	return texture(skydot_cell_mask, (cell + 0.5) / skydot_mask_size).r > 0.5;
}
