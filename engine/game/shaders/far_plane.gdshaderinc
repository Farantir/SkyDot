/* Sky objects sit just in front of the far plane (depth is reversed).
 *
 * Included by clouds.gdshader and sprite.gdshaderinc.
 */

void vertex() {
	POSITION = PROJECTION_MATRIX * MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
	POSITION.z = POSITION.w * 0.000001;
}
