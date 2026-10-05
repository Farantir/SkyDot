/* The sky shader (see render/weather.hpp).
 *
 * A whole shader that SkydotWeather::build() in render/weather.cpp loads as it is, as the
 * environment's sky; SkydotWeather sets the uniforms.
 */

shader_type sky;
uniform vec3 upper;
uniform vec3 horizon;
uniform vec3 lower;
uniform vec3 flash_color;
uniform float flash = 0.0;
void sky() {
	float y = EYEDIR.y;
	// The horizon colour reaches well up: at 53 degrees the game's sky is
	// about halfway to the upper colour (comparison shot ref11).
	vec3 c = y >= 0.0 ? mix(horizon, upper, clamp(y * y, 0.0, 1.0))
	                  : mix(horizon, lower, sqrt(clamp(-y, 0.0, 1.0)));
	COLOR = c + flash_color * flash;
}
