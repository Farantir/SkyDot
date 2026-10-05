/* The moons: they cover what is behind.
 *
 * The sprite shader (sprite.gdshaderinc) blended as blend_mix.
 */
shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_disabled, fog_disabled, shadows_disabled;

#include "res://shaders/sprite.gdshaderinc"
