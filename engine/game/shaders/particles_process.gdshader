/* The process shader every particle system shares (see render/particles.hpp).
 *
 * A whole shader that SkydotMaterials::particles_process_shader() in render/materials.cpp
 * loads as it is.
 */
shader_type particles;
render_mode disable_force;

// Emitter volume, in the emitter's space, placed in the system's space by
// emitter_xform. Mesh emitters give points already in the system's space.
uniform int emitter_kind = 0; // 0 box, 1 sphere, 2 cylinder, 3 mesh points
uniform vec3 emitter_size = vec3(0.0);
uniform mat4 emitter_xform = mat4(1.0);
uniform sampler2D points_tex : filter_nearest;
uniform sampler2D normals_tex : filter_nearest;
uniform int point_count = 0;
uniform int point_velocity = 0; // 0 along normals, 1 random, 2 along point_axis
uniform vec3 point_axis = vec3(0.0, 0.0, 1.0);
// (value, variation): magnitudes vary by +-variation/2, angles by +-variation.
uniform vec2 speed = vec2(0.0);
uniform vec2 declination = vec2(0.0);
uniform vec2 planar = vec2(0.0);
uniform vec2 radius = vec2(1.0, 0.0);
uniform vec2 life = vec2(1.0, 0.0);
uniform vec4 color = vec4(1.0);
uniform vec2 spin = vec2(0.0);
uniform vec2 spin_angle = vec2(0.0);
uniform bool spin_random_sign = false;
// Planar: xyz is the acceleration. Spherical: xyz is the centre, and
// params.x the strength. params: y decay per unit, z 1 if spherical,
// w turbulence as a fraction of the strength.
uniform int gravity_count = 0;
uniform vec4 gravity_vector[4];
uniform vec4 gravity_params[4];
// Drag: xyz the axis in the particles' space (zero for every direction), w
// the fraction of that velocity kept per second. centre: xyz the drag node,
// w its range (negative for unlimited); params.x the range falloff.
uniform int drag_count = 0;
uniform vec4 drag_axis[4];
uniform vec4 drag_center[4];
uniform vec4 drag_params[4];
uniform sampler2D scale_curve : filter_linear, repeat_disable;
uniform float scale_duration = 0.0;
uniform vec2 grow_fade = vec2(0.0);
uniform float base_scale = 1.0;
uniform bool use_simple_color = false;
uniform vec2 fade = vec2(0.0);
uniform vec4 color_stops = vec4(0.0); // colour 1 end, 2 start, 2 end, 3 start
uniform vec4 color1 = vec4(1.0);
uniform vec4 color2 = vec4(1.0);
uniform vec4 color3 = vec4(1.0);
uniform sampler2D color_ramp : filter_linear, repeat_disable;
uniform bool use_color_ramp = false;
uniform int frames = 0;
uniform vec4 subtex = vec4(0.0);  // start, start variation, end, loop start
uniform vec4 subtex2 = vec4(0.0); // loop start variation, frame count, its variation
uniform bool world_space = true;

float rand(inout uint seed) {
	seed = seed * 747796405u + 2891336453u;
	uint w = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
	return float((w >> 22u) ^ w) / 4294967295.0;
}

// ud1 (USERDATA1): age, spin speed, radius, emission scale.
// ud2 (USERDATA2): first frame, loop start, frames over the lifetime.
// Helpers cannot see the built-ins, so they come in as arguments.
void look(float age, float lifetime, vec4 ud1, vec4 ud2, inout mat4 xf, inout vec4 custom,
		out vec4 col) {
	float t = clamp(age / lifetime, 0.0, 1.0);
	custom.y = t;
	float s = ud1.z * base_scale;
	if (scale_duration > 0.0) {
		s *= texture(scale_curve, vec2(clamp(age / scale_duration, 0.0, 1.0), 0.5)).r;
	}
	if (grow_fade.x > 0.0) {
		s *= clamp(age / grow_fade.x, 0.0, 1.0);
	}
	if (grow_fade.y > 0.0) {
		s *= clamp((lifetime - age) / grow_fade.y, 0.0, 1.0);
	}
	float quad = 2.0 * s * ud1.w;
	xf[0] = vec4(quad, 0.0, 0.0, 0.0);
	xf[1] = vec4(0.0, quad, 0.0, 0.0);
	xf[2] = vec4(0.0, 0.0, quad, 0.0);

	vec4 c = color;
	if (use_simple_color) {
		vec4 k;
		if (t <= color_stops.x) {
			k = color1;
		} else if (t < color_stops.y) {
			k = mix(color1, color2, (t - color_stops.x) / max(color_stops.y - color_stops.x, 1e-5));
		} else if (t <= color_stops.z) {
			k = color2;
		} else if (t < color_stops.w) {
			k = mix(color2, color3, (t - color_stops.z) / max(color_stops.w - color_stops.z, 1e-5));
		} else {
			k = color3;
		}
		float f = 1.0;
		if (fade.x > 0.0) {
			f *= clamp(t / fade.x, 0.0, 1.0);
		}
		if (fade.y > 0.0) {
			f *= clamp((1.0 - t) / fade.y, 0.0, 1.0);
		}
		c *= vec4(k.rgb, k.a * f);
	}
	if (use_color_ramp) {
		c *= texture(color_ramp, vec2(t, 0.5));
	}
	col = c;

	if (frames > 0) {
		float f = ud2.x + t * ud2.z;
		float last = subtex.z > 0.0 ? min(subtex.z, float(frames - 1)) : float(frames - 1);
		float loop_start = min(ud2.y, last);
		if (f > last + 1.0) {
			f = loop_start + mod(f - loop_start, last - loop_start + 1.0);
		}
		custom.z = clamp(floor(f), 0.0, float(frames - 1));
	}
}

void start() {
	uint seed = uint(NUMBER) * 1973u + uint(RANDOM_SEED) * 9277u + 26699u;
	float lifetime = max(life.x + (rand(seed) - 0.5) * life.y, 0.01);
	CUSTOM = vec4(0.0, 0.0, 0.0, lifetime);
	float r = max(radius.x + (rand(seed) - 0.5) * radius.y, 0.0);
	float w = spin.x + (rand(seed) * 2.0 - 1.0) * spin.y;
	if (spin_random_sign && rand(seed) < 0.5) {
		w = -w;
	}
	CUSTOM.x = spin_angle.x + (rand(seed) * 2.0 - 1.0) * spin_angle.y;
	USERDATA1 = vec4(0.0, w, r, world_space ? length(EMISSION_TRANSFORM[0].xyz) : 1.0);
	USERDATA2 = vec4(subtex.x + rand(seed) * subtex.y, subtex.w + rand(seed) * subtex2.x,
			max(subtex2.y + (rand(seed) - 0.5) * subtex2.z, 0.0), 0.0);

	if (RESTART_POSITION) {
		vec3 p = vec3(0.0);
		vec3 n = vec3(0.0, 0.0, 1.0);
		if (emitter_kind == 0) {
			p = (vec3(rand(seed), rand(seed), rand(seed)) - 0.5) * emitter_size;
		} else if (emitter_kind == 1) {
			vec3 d = normalize(vec3(rand(seed), rand(seed), rand(seed)) * 2.0 - 1.0 + 1e-5);
			p = d * emitter_size.x * pow(rand(seed), 1.0 / 3.0);
		} else if (emitter_kind == 2) {
			float a = rand(seed) * TAU;
			float d = emitter_size.x * sqrt(rand(seed));
			p = vec3(cos(a) * d, sin(a) * d, (rand(seed) - 0.5) * emitter_size.y);
		} else if (point_count > 0) {
			int i = min(int(rand(seed) * float(point_count)), point_count - 1);
			ivec2 texel = ivec2(i % 256, i / 256);
			p = texelFetch(points_tex, texel, 0).xyz;
			n = texelFetch(normals_tex, texel, 0).xyz;
		}
		vec3 dir;
		if (emitter_kind == 3 && point_velocity == 0) {
			dir = length(n) > 0.0 ? normalize(n) : vec3(0.0, 0.0, 1.0);
		} else if (emitter_kind == 3 && point_velocity == 1) {
			dir = normalize(vec3(rand(seed), rand(seed), rand(seed)) * 2.0 - 1.0 + 1e-5);
		} else if (emitter_kind == 3) {
			dir = length(point_axis) > 0.0 ? normalize(point_axis) : vec3(0.0, 0.0, 1.0);
		} else {
			float d = declination.x + (rand(seed) * 2.0 - 1.0) * declination.y;
			float a = planar.x + (rand(seed) * 2.0 - 1.0) * planar.y;
			dir = vec3(sin(d) * cos(a), sin(d) * sin(a), cos(d));
		}
		float v = speed.x + (rand(seed) - 0.5) * speed.y;
		vec3 pos = emitter_kind == 3 ? p : (emitter_xform * vec4(p, 1.0)).xyz;
		vec3 vel = emitter_kind == 3 ? dir * v : (emitter_xform * vec4(dir * v, 0.0)).xyz;
		if (world_space) {
			pos = (EMISSION_TRANSFORM * vec4(pos, 1.0)).xyz;
			vel = (EMISSION_TRANSFORM * vec4(vel, 0.0)).xyz;
		}
		TRANSFORM = mat4(1.0);
		TRANSFORM[3].xyz = pos;
		VELOCITY = vel;
	}
	look(0.0, lifetime, USERDATA1, USERDATA2, TRANSFORM, CUSTOM, COLOR);
}

void process() {
	float age = USERDATA1.x + DELTA;
	USERDATA1.x = age;
	float lifetime = CUSTOM.w;
	if (age >= lifetime) {
		ACTIVE = false;
	}
	vec3 pos = TRANSFORM[3].xyz;
	vec3 accel = vec3(0.0);
	for (int i = 0; i < gravity_count; i++) {
		vec4 g = gravity_params[i];
		vec3 f;
		if (g.z > 0.5) {
			vec3 d = pos - gravity_vector[i].xyz;
			float dist = length(d);
			f = dist > 1e-5 ? d / dist * g.x : vec3(0.0);
			f *= exp(-g.y * dist);
		} else {
			f = gravity_vector[i].xyz;
		}
		if (g.w > 0.0) {
			uint seed = uint(NUMBER) * 7919u + uint(age * 8.0) * 104729u + uint(i);
			f += (vec3(rand(seed), rand(seed), rand(seed)) * 2.0 - 1.0) * g.w * length(f);
		}
		accel += f;
	}
	VELOCITY += accel * DELTA;
	for (int i = 0; i < drag_count; i++) {
		float keep = pow(drag_axis[i].w, DELTA);
		if (drag_center[i].w >= 0.0) {
			float beyond = length(pos - drag_center[i].xyz) - drag_center[i].w;
			float reach = beyond <= 0.0 ? 1.0
					: clamp(1.0 - beyond / max(drag_params[i].x, 1e-5), 0.0, 1.0);
			keep = mix(1.0, keep, reach);
		}
		vec3 a = drag_axis[i].xyz;
		if (dot(a, a) > 0.0) {
			vec3 along = a * dot(VELOCITY, a);
			VELOCITY += along * (keep - 1.0);
		} else {
			VELOCITY *= keep;
		}
	}
	CUSTOM.x += USERDATA1.y * DELTA;
	look(age, lifetime, USERDATA1, USERDATA2, TRANSFORM, CUSTOM, COLOR);
}
