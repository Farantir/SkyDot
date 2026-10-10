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

#include "res://shaders/sky_dome.gdshaderinc"

void sky() {
	COLOR = sky_dome(EYEDIR, upper, horizon, lower) + flash_color * flash;
}
