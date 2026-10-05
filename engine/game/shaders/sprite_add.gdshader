/* Stars, the sun and its glare: they add light.
 *
 * The sprite shader (sprite.gdshaderinc) blended as blend_add.
 */
shader_type spatial;
render_mode unshaded, blend_add, depth_draw_never, cull_disabled, fog_disabled, shadows_disabled;

#include "res://shaders/sprite.gdshaderinc"
