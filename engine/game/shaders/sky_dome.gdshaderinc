/* The game's sky dome: its colour along a direction (see render/weather.hpp).
 *
 * Included by sky.gdshader, and by water.gdshader, which reflects the dome as
 * the game's water reflects its sky cubemap.
 */

// The game's sky dome (meshes/sky/atmosphere.nif) weights the three colours
// by its vertex colours: r horizon, g lower, b upper. Its rings, by the
// elevation they sit at on the mesh (degrees) and their weights. The lower
// colour is the band from about 15 to 30 degrees, not below the horizon.
const int RINGS = 14;
const float RING_ELEVATION[RINGS] = float[](0.0, 2.1, 3.6, 5.6, 9.7, 14.5, 18.0, 19.4, 26.8, 34.9, 46.9, 59.1, 73.7, 90.0);
const vec3 RING_WEIGHTS[RINGS] = vec3[](
	vec3(1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0), vec3(0.933, 0.071, 0.0), vec3(0.804, 0.22, 0.0),
	vec3(0.49, 0.573, 0.0), vec3(0.157, 0.91, 0.0), vec3(0.02, 1.0, 0.016), vec3(0.008, 1.0, 0.02),
	vec3(0.0, 0.769, 0.306), vec3(0.0, 0.471, 0.584), vec3(0.0, 0.184, 0.831), vec3(0.0, 0.063, 0.941),
	vec3(0.0, 0.012, 0.984), vec3(0.0, 0.0, 1.0));
// The dome as seen is 1.2 times flatter than the mesh (the camera does not sit
// at its centre, or the game scales it): fitted on game shots of two clear
// weathers at four hours with volumetric lighting off (comparison-renders/
// game-refs-2026-10-10/o_*, 2026-10-09).
const float DOME_FLATTEN = 1.2;

vec3 dome_weights(float elevation) {
	for (int i = 1; i < RINGS; i++) {
		if (elevation <= RING_ELEVATION[i]) {
			float t = (elevation - RING_ELEVATION[i - 1]) / (RING_ELEVATION[i] - RING_ELEVATION[i - 1]);
			return mix(RING_WEIGHTS[i - 1], RING_WEIGHTS[i], t);
		}
	}
	return RING_WEIGHTS[RINGS - 1];
}

// The dome's colour along a direction (y up); below the horizon the horizon
// colour runs into the lower one.
vec3 sky_dome(vec3 direction, vec3 upper_color, vec3 horizon_color, vec3 lower_color) {
	float y = direction.y;
	if (y >= 0.0) {
		float elevation = degrees(atan(y, DOME_FLATTEN * sqrt(max(1.0 - y * y, 0.0))));
		vec3 w = dome_weights(elevation);
		return w.x * horizon_color + w.y * lower_color + w.z * upper_color;
	}
	return mix(horizon_color, lower_color, sqrt(clamp(-y, 0.0, 1.0)));
}
