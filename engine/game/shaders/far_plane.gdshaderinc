/* Sky objects sit just in front of the far plane (depth is reversed).
 *
 * make_shader() in render/weather.cpp replaces the line %FAR% of clouds.gdshader and
 * sprite.gdshader with this.
 *
 * This comment is not part of the shader code: the loader cuts it off (README.md).
 */

void vertex() {
	POSITION = PROJECTION_MATRIX * MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
	POSITION.z = POSITION.w * 0.000001;
}
