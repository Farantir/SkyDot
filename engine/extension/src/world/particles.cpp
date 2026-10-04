// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/particles.hpp"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/packed_vector4_array.hpp>
#include <godot_cpp/variant/projection.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

using godot::Array;
using godot::Dictionary;
using godot::Ref;
using godot::String;
using godot::Transform3D;
using godot::Variant;
using godot::Vector3;
using godot::Vector4;

namespace skydot {

namespace {

constexpr std::int64_t k_points_width = 256;
constexpr std::int64_t k_max_points = k_points_width * 64;
constexpr std::int64_t k_max_particles = 2048;
constexpr int k_max_gravity = 4;
constexpr int k_max_drags = 4;
constexpr int k_curve_width = 64;

constexpr const char* k_process_shader = R"(shader_type particles;
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
)";

godot::Vector2 pair(double a, double b) {
    return godot::Vector2(static_cast<float>(a), static_cast<float>(b));
}

double num(const Dictionary& d, const char* key, double fallback = 0.0) {
    const Variant v = d.get(key, fallback);
    return v.get_type() == Variant::FLOAT || v.get_type() == Variant::INT ? static_cast<double>(v)
                                                                            : fallback;
}

Vector3 vec3(const Dictionary& d, const char* key, Vector3 fallback = Vector3()) {
    const Variant v = d.get(key, Variant());
    if (v.get_type() != Variant::ARRAY || Array(v).size() < 3) {
        return fallback;
    }
    const Array a = v;
    return Vector3(static_cast<float>(static_cast<double>(a[0])),
                   static_cast<float>(static_cast<double>(a[1])),
                   static_cast<float>(static_cast<double>(a[2])));
}

godot::Color color(const Variant& v, godot::Color fallback = godot::Color(1, 1, 1, 1)) {
    if (v.get_type() != Variant::ARRAY || Array(v).size() < 4) {
        return fallback;
    }
    const Array a = v;
    // Some files leave denormal garbage in unused alphas; treat it as zero.
    auto channel = [](const Variant& x) {
        const double f = static_cast<double>(x);
        return static_cast<float>(std::fabs(f) < 1e-30 ? 0.0 : f);
    };
    return godot::Color(channel(a[0]), channel(a[1]), channel(a[2]), channel(a[3]));
}

godot::Node3D* node3d(const std::unordered_map<std::int64_t, godot::Node*>& nodes,
                      const Dictionary& d, const char* key) {
    const Variant v = d.get(key, Variant());
    if (v.get_type() != Variant::INT && v.get_type() != Variant::FLOAT) {
        return nullptr;
    }
    const auto it = nodes.find(static_cast<std::int64_t>(v));
    return it != nodes.end() ? godot::Object::cast_to<godot::Node3D>(it->second) : nullptr;
}

/// A one-row R32F texture of `values`, for curves.
Ref<godot::ImageTexture> curve_texture(const std::vector<float>& values) {
    godot::PackedByteArray bytes;
    bytes.resize(static_cast<std::int64_t>(values.size() * sizeof(float)));
    std::memcpy(bytes.ptrw(), values.data(), values.size() * sizeof(float));
    const Ref<godot::Image> image = godot::Image::create_from_data(
        static_cast<std::int32_t>(values.size()), 1, false, godot::Image::FORMAT_RF, bytes);
    return godot::ImageTexture::create_from_image(image);
}

Ref<godot::ImageTexture> vector_texture(const std::vector<Vector3>& points) {
    const std::int64_t count = static_cast<std::int64_t>(points.size());
    const std::int64_t width = std::min(count, k_points_width);
    const std::int64_t height = (count + k_points_width - 1) / k_points_width;
    std::vector<float> data(static_cast<std::size_t>(width * height * 3), 0.0f);
    for (std::size_t i = 0; i < points.size(); ++i) {
        data[i * 3] = points[i].x;
        data[i * 3 + 1] = points[i].y;
        data[i * 3 + 2] = points[i].z;
    }
    godot::PackedByteArray bytes;
    bytes.resize(static_cast<std::int64_t>(data.size() * sizeof(float)));
    std::memcpy(bytes.ptrw(), data.data(), data.size() * sizeof(float));
    const Ref<godot::Image> image = godot::Image::create_from_data(
        static_cast<std::int32_t>(width), static_cast<std::int32_t>(height), false,
        godot::Image::FORMAT_RGBF, bytes);
    return godot::ImageTexture::create_from_image(image);
}

/// Uniform scale of a basis (converter models only scale uniformly).
double scale_of(const godot::Basis& basis) {
    return static_cast<double>(basis.get_column(0).length());
}

} // namespace

const char* SkydotParticles::process_shader_code() { return k_process_shader; }

void SkydotParticles::_bind_methods() {
    using godot::D_METHOD;
    godot::ClassDB::bind_method(D_METHOD("set_birth_rate", "rate"), &SkydotParticles::set_birth_rate);
    godot::ClassDB::bind_method(D_METHOD("set_active", "active"), &SkydotParticles::set_active);
    godot::ClassDB::bind_method(D_METHOD("get_emitter_count"), &SkydotParticles::get_emitter_count);
    godot::ClassDB::bind_method(D_METHOD("get_birth_rate"), &SkydotParticles::get_birth_rate);
}

void SkydotParticles::setup(const Dictionary& block, const Ref<godot::ShaderMaterial>& draw,
                            const Ref<SkydotMaterials>& materials,
                            const std::unordered_map<std::int64_t, godot::Node*>& nodes) {
    block_ = block;
    draw_ = draw;
    materials_ = materials;
    const Variant emitters = block.get("emitters", Variant());
    if (emitters.get_type() == Variant::ARRAY) {
        const Array list = emitters;
        for (std::int64_t i = 0; i < list.size(); ++i) {
            if (list[i].get_type() != Variant::DICTIONARY) {
                continue;
            }
            Emitter em;
            em.block = list[i];
            em.node = node3d(nodes, em.block, "node");
            em.peak_rate = num(em.block, "birth_rate");
            const Variant meshes = em.block.get("meshes", Variant());
            if (meshes.get_type() == Variant::ARRAY) {
                const Array m = meshes;
                for (std::int64_t j = 0; j < m.size(); ++j) {
                    const auto it = nodes.find(static_cast<std::int64_t>(m[j]));
                    if (it != nodes.end()) {
                        if (auto* n = godot::Object::cast_to<godot::Node3D>(it->second)) {
                            em.meshes.push_back(n);
                        }
                    }
                }
            }
            emitters_.push_back(std::move(em));
        }
    }
    const Variant gravity = block.get("gravity", Variant());
    if (gravity.get_type() == Variant::ARRAY) {
        const Array list = gravity;
        for (std::int64_t i = 0; i < list.size() && gravity_.size() < k_max_gravity; ++i) {
            if (list[i].get_type() == Variant::DICTIONARY) {
                gravity_.push_back(Field{list[i], node3d(nodes, list[i], "node")});
            }
        }
    }
    const Variant drags = block.get("drags", Variant());
    if (drags.get_type() == Variant::ARRAY) {
        const Array list = drags;
        for (std::int64_t i = 0; i < list.size() && drags_.size() < k_max_drags; ++i) {
            if (list[i].get_type() == Variant::DICTIONARY) {
                drags_.push_back(Field{list[i], node3d(nodes, list[i], "node")});
            }
        }
    }
}

std::int64_t SkydotParticles::get_emitter_count() const {
    return static_cast<std::int64_t>(emitters_.size());
}

double SkydotParticles::get_birth_rate() const {
    return emitters_.empty() ? 0.0 : emitters_.front().peak_rate;
}

void SkydotParticles::_ready() { build(); }

void SkydotParticles::build() {
    if (built_) {
        return;
    }
    built_ = true;
    for (Emitter& em : emitters_) {
        build_emitter(em);
    }
    update_emission();
}

void SkydotParticles::build_emitter(Emitter& em) {
    const Dictionary& e = em.block;
    const bool world_space = static_cast<bool>(block_.get("world_space", true));
    const double life = num(e, "life_span", 1.0);
    const double life_var = num(e, "life_span_variation");
    const double max_life = std::max(life + life_var / 2.0, 0.05);
    const std::int64_t cap = static_cast<std::int64_t>(num(block_, "max_particles"));
    std::int64_t amount = static_cast<std::int64_t>(std::ceil(em.peak_rate * max_life));
    amount = std::clamp<std::int64_t>(amount, 1, cap > 0 ? std::min(cap, k_max_particles)
                                                         : k_max_particles);

    Ref<godot::ShaderMaterial> process;
    process.instantiate();
    process->set_shader(materials_->particles_process_shader());

    // The space particles live in: the world, or this system's node.
    const Transform3D system = get_global_transform();
    const Transform3D space = world_space ? Transform3D() : system;
    const Transform3D to_space = space.affine_inverse();
    const Transform3D to_system = system.affine_inverse();
    const double space_scale = world_space ? 1.0 : scale_of(system.basis);

    const godot::String kind = e.get("kind", String("box"));
    const Vector3 size = vec3(e, "size");
    process->set_shader_parameter("emitter_size", size);
    Transform3D emitter;
    if (em.node != nullptr) {
        emitter = to_system * em.node->get_global_transform();
    }
    process->set_shader_parameter("emitter_xform", godot::Projection(emitter));
    double extent = 0.0;
    if (kind == "sphere") {
        process->set_shader_parameter("emitter_kind", 1);
        extent = static_cast<double>(size.x);
    } else if (kind == "cylinder") {
        process->set_shader_parameter("emitter_kind", 2);
        extent = static_cast<double>(std::max(size.x, size.y));
    } else if (kind == "mesh") {
        process->set_shader_parameter("emitter_kind", 3);
        std::vector<Vector3> points;
        std::vector<Vector3> normals;
        for (godot::Node3D* node : em.meshes) {
            auto* instance = godot::Object::cast_to<godot::MeshInstance3D>(node);
            if (instance == nullptr) {
                for (std::int32_t i = 0; i < node->get_child_count() && instance == nullptr; ++i) {
                    instance = godot::Object::cast_to<godot::MeshInstance3D>(node->get_child(i));
                }
            }
            if (instance == nullptr || instance->get_mesh().is_null()) {
                continue;
            }
            const Ref<godot::Mesh> mesh = instance->get_mesh();
            const Transform3D to_points = to_system * instance->get_global_transform();
            const godot::Basis normal_basis = to_points.basis.inverse().transposed();
            for (std::int32_t s = 0; s < mesh->get_surface_count(); ++s) {
                const Array arrays = mesh->surface_get_arrays(s);
                const godot::PackedVector3Array v = arrays[godot::Mesh::ARRAY_VERTEX];
                const Variant nv = arrays[godot::Mesh::ARRAY_NORMAL];
                const godot::PackedVector3Array n =
                    nv.get_type() == Variant::PACKED_VECTOR3_ARRAY ? godot::PackedVector3Array(nv)
                                                                   : godot::PackedVector3Array();
                for (std::int64_t i = 0; i < v.size() &&
                                         static_cast<std::int64_t>(points.size()) < k_max_points;
                     ++i) {
                    points.push_back(to_points.xform(v[i]));
                    normals.push_back(i < n.size() ? (normal_basis.xform(n[i])).normalized()
                                                   : Vector3(0, 0, 1));
                    extent = std::max(extent, static_cast<double>(points.back().length()));
                }
            }
        }
        if (!points.empty()) {
            process->set_shader_parameter("points_tex", vector_texture(points));
            process->set_shader_parameter("normals_tex", vector_texture(normals));
            process->set_shader_parameter("point_count", static_cast<std::int64_t>(points.size()));
        }
        process->set_shader_parameter("point_velocity",
                                      static_cast<std::int64_t>(num(e, "mesh_velocity")));
        process->set_shader_parameter("point_axis", vec3(e, "mesh_axis", Vector3(0, 0, 1)));
    } else {
        process->set_shader_parameter("emitter_kind", 0);
        extent = static_cast<double>(size.length()) / 2.0;
    }
    extent += static_cast<double>(emitter.origin.length());

    const double speed = num(e, "speed");
    const double speed_var = num(e, "speed_variation");
    const double radius = num(e, "radius", 1.0);
    const double radius_var = num(e, "radius_variation");
    process->set_shader_parameter("speed", pair(speed, speed_var));
    process->set_shader_parameter("declination",
                                  pair(num(e, "declination"), num(e, "declination_variation")));
    process->set_shader_parameter("planar", pair(num(e, "planar_angle"),
                                                           num(e, "planar_angle_variation")));
    process->set_shader_parameter("radius", pair(radius, radius_var));
    process->set_shader_parameter("life", pair(life, life_var));
    process->set_shader_parameter("color", shader_rgba(color(e.get("color", Variant()))));
    process->set_shader_parameter("world_space", world_space);

    const Variant rotation = block_.get("rotation", Variant());
    if (rotation.get_type() == Variant::DICTIONARY) {
        const Dictionary r = rotation;
        process->set_shader_parameter("spin", pair(num(r, "speed"), num(r, "speed_variation")));
        process->set_shader_parameter("spin_angle", pair(num(r, "angle"), num(r, "angle_variation")));
        process->set_shader_parameter("spin_random_sign", static_cast<bool>(r.get("random_sign", false)));
    }

    // Gravity in the particles' space: planar forces as accelerations,
    // spherical ones as a centre and strength.
    godot::PackedVector4Array vectors;
    godot::PackedVector4Array params;
    double max_accel = 0.0;
    for (const Field& g : gravity_) {
        const double strength = num(g.block, "strength");
        const double decay = num(g.block, "decay");
        const bool spherical = static_cast<bool>(g.block.get("spherical", false));
        const double turbulence = num(g.block, "turbulence");
        Transform3D object = g.node != nullptr ? g.node->get_global_transform() : system;
        const double world_scale = scale_of(object.basis);
        const double unit = world_scale / space_scale; // space units per NIF unit
        Vector3 axis = vec3(g.block, "axis", Vector3(0, 0, 1));
        Vector3 world_axis;
        if (static_cast<bool>(g.block.get("world_aligned", false))) {
            // A Z-up world axis, in Godot's Y-up world.
            world_axis = Vector3(axis.x, axis.z, -axis.y).normalized() * static_cast<float>(world_scale);
        } else {
            world_axis = object.basis.xform(axis);
        }
        if (spherical) {
            vectors.push_back(Vector4(to_space.xform(object.origin).x, to_space.xform(object.origin).y,
                                      to_space.xform(object.origin).z, 0.0f));
            params.push_back(Vector4(static_cast<float>(strength * unit),
                                     static_cast<float>(unit > 0.0 ? decay / unit : 0.0), 1.0f,
                                     static_cast<float>(turbulence)));
        } else {
            const Vector3 a = to_space.basis.xform(world_axis) * static_cast<float>(strength);
            vectors.push_back(Vector4(a.x, a.y, a.z, 0.0f));
            params.push_back(Vector4(1.0f, 0.0f, 0.0f, static_cast<float>(turbulence)));
        }
        max_accel += std::fabs(strength);
    }
    process->set_shader_parameter("gravity_count", static_cast<std::int64_t>(vectors.size()));
    if (!vectors.is_empty()) {
        while (vectors.size() < k_max_gravity) {
            vectors.push_back(Vector4());
            params.push_back(Vector4());
        }
        process->set_shader_parameter("gravity_vector", vectors);
        process->set_shader_parameter("gravity_params", params);
    }
    // Drag percentages are taken per 1/60 s frame: read per second, vanilla
    // waterfall spray flies hundreds of units off the water. Calibrated by eye
    // against the game; Gamebryo's exact rule is unknown.
    godot::PackedVector4Array drag_axes;
    godot::PackedVector4Array drag_centers;
    godot::PackedVector4Array drag_params;
    const auto add_drag = [&](const Field* d, double percentage) {
        const double keep = std::pow(std::clamp(1.0 - percentage, 0.0, 1.0), 60.0);
        Vector3 axis;
        Transform3D object = system;
        double range = -1.0;
        double falloff = 0.0;
        if (d != nullptr) {
            object = d->node != nullptr ? d->node->get_global_transform() : system;
            const Vector3 local = vec3(d->block, "axis", Vector3());
            if (local.length_squared() > 0.0f) {
                axis = to_space.basis.xform(object.basis.xform(local)).normalized();
            }
            const double unit = scale_of(object.basis) / space_scale;
            const double r = num(d->block, "range", -1.0);
            if (r >= 0.0 && r < 1e30) {
                range = r * unit;
                falloff = std::min(num(d->block, "range_falloff"), 1e30) * unit;
            }
        }
        const Vector3 centre = to_space.xform(object.origin);
        drag_axes.push_back(Vector4(axis.x, axis.y, axis.z, static_cast<float>(keep)));
        drag_centers.push_back(Vector4(centre.x, centre.y, centre.z, static_cast<float>(range)));
        drag_params.push_back(Vector4(static_cast<float>(falloff), 0.0f, 0.0f, 0.0f));
    };
    for (const Field& d : drags_) {
        add_drag(&d, num(d.block, "percentage"));
    }
    if (drags_.empty() && num(block_, "drag") > 0.0) {
        add_drag(nullptr, num(block_, "drag")); // packs from before per-axis drag
    }
    process->set_shader_parameter("drag_count", static_cast<std::int64_t>(drag_axes.size()));
    if (!drag_axes.is_empty()) {
        while (drag_axes.size() < k_max_drags) {
            drag_axes.push_back(Vector4(0, 0, 0, 1));
            drag_centers.push_back(Vector4(0, 0, 0, -1));
            drag_params.push_back(Vector4());
        }
        process->set_shader_parameter("drag_axis", drag_axes);
        process->set_shader_parameter("drag_center", drag_centers);
        process->set_shader_parameter("drag_params", drag_params);
    }

    double max_scale = 1.0;
    const Variant scales = block_.get("scales", Variant());
    if (scales.get_type() == Variant::ARRAY && Array(scales).size() > 0) {
        const Array a = scales;
        std::vector<float> values;
        for (std::int64_t i = 0; i < a.size(); ++i) {
            values.push_back(static_cast<float>(static_cast<double>(a[i])));
            max_scale = std::max(max_scale, static_cast<double>(values.back()));
        }
        // Resampled to a fixed width; the converter's curve is 60 per second.
        std::vector<float> curve(k_curve_width);
        for (int i = 0; i < k_curve_width; ++i) {
            const double x = static_cast<double>(i) / (k_curve_width - 1) * static_cast<double>(values.size() - 1);
            const std::size_t j = static_cast<std::size_t>(x);
            const std::size_t k = std::min(j + 1, values.size() - 1);
            curve[static_cast<std::size_t>(i)] =
                static_cast<float>(static_cast<double>(values[j]) +
                                   static_cast<double>(values[k] - values[j]) * (x - static_cast<double>(j)));
        }
        process->set_shader_parameter("scale_curve", curve_texture(curve));
        process->set_shader_parameter("scale_duration", static_cast<double>(values.size()) / 60.0);
    }
    process->set_shader_parameter("grow_fade",
                                  pair(num(block_, "grow_time"), num(block_, "fade_time")));
    process->set_shader_parameter("base_scale", num(block_, "base_scale", 1.0));

    const Variant simple = block_.get("simple_color", Variant());
    if (simple.get_type() == Variant::DICTIONARY) {
        const Dictionary c = simple;
        process->set_shader_parameter("use_simple_color", true);
        process->set_shader_parameter("fade", pair(num(c, "fade_in"), num(c, "fade_out")));
        process->set_shader_parameter(
            "color_stops", Vector4(static_cast<float>(num(c, "color1_end")),
                                   static_cast<float>(num(c, "color2_start")),
                                   static_cast<float>(num(c, "color2_end")),
                                   static_cast<float>(num(c, "color3_start"))));
        const Variant colors = c.get("colors", Variant());
        if (colors.get_type() == Variant::ARRAY && Array(colors).size() >= 3) {
            const Array list = colors;
            process->set_shader_parameter("color1", shader_rgba(color(list[0])));
            process->set_shader_parameter("color2", shader_rgba(color(list[1])));
            process->set_shader_parameter("color3", shader_rgba(color(list[2])));
        }
    }
    const Variant keys = block_.get("color_keys", Variant());
    if (keys.get_type() == Variant::ARRAY && Array(keys).size() > 0) {
        // Colour keys, resampled into a ramp over the lifetime.
        const Array list = keys;
        std::vector<std::pair<double, godot::Color>> stops;
        for (std::int64_t i = 0; i < list.size(); ++i) {
            if (list[i].get_type() == Variant::ARRAY && Array(list[i]).size() >= 5) {
                const Array k = list[i];
                stops.emplace_back(static_cast<double>(k[0]),
                                   godot::Color(static_cast<float>(static_cast<double>(k[1])),
                                                static_cast<float>(static_cast<double>(k[2])),
                                                static_cast<float>(static_cast<double>(k[3])),
                                                static_cast<float>(static_cast<double>(k[4]))));
            }
        }
        if (!stops.empty()) {
            const Ref<godot::Image> ramp =
                godot::Image::create_empty(k_curve_width, 1, false, godot::Image::FORMAT_RGBAF);
            for (int i = 0; i < k_curve_width; ++i) {
                const double t = static_cast<double>(i) / (k_curve_width - 1);
                godot::Color c = stops.back().second;
                if (t <= stops.front().first) {
                    c = stops.front().second;
                } else {
                    for (std::size_t j = 0; j + 1 < stops.size(); ++j) {
                        if (t >= stops[j].first && t <= stops[j + 1].first) {
                            const double span = stops[j + 1].first - stops[j].first;
                            c = stops[j].second.lerp(stops[j + 1].second,
                                                     span > 0.0 ? static_cast<float>((t - stops[j].first) / span) : 0.0f);
                            break;
                        }
                    }
                }
                ramp->set_pixel(i, 0, c);
            }
            process->set_shader_parameter("color_ramp", godot::ImageTexture::create_from_image(ramp));
            process->set_shader_parameter("use_color_ramp", true);
        }
    }

    const Variant rects = block_.get("subtex_offsets", Variant());
    const std::int64_t frames = rects.get_type() == Variant::ARRAY ? Array(rects).size() : 0;
    const Variant subtex = block_.get("subtex", Variant());
    if (frames > 0 && subtex.get_type() == Variant::DICTIONARY) {
        const Dictionary s = subtex;
        process->set_shader_parameter("frames", std::min<std::int64_t>(frames, 64));
        process->set_shader_parameter(
            "subtex", Vector4(static_cast<float>(num(s, "start")), static_cast<float>(num(s, "start_variation")),
                              static_cast<float>(num(s, "end")), static_cast<float>(num(s, "loop_start"))));
        process->set_shader_parameter(
            "subtex2", Vector4(static_cast<float>(num(s, "loop_start_variation")),
                               static_cast<float>(num(s, "frame_count")),
                               static_cast<float>(num(s, "frame_count_variation")), 0.0f));
    }

    auto* particles = memnew(godot::GPUParticles3D);
    particles->set_name("Emitter");
    particles->set_amount(static_cast<std::int32_t>(amount));
    particles->set_lifetime(max_life);
    particles->set_pre_process_time(std::min(max_life, 10.0));
    particles->set_use_local_coordinates(!world_space);
    particles->set_process_material(process);
    particles->set_draw_order(godot::GPUParticles3D::DRAW_ORDER_VIEW_DEPTH);
    Ref<godot::QuadMesh> quad;
    quad.instantiate();
    quad->set_size(godot::Vector2(1, 1));
    quad->set_material(draw_);
    particles->set_draw_pass_mesh(0, quad);

    // Bounds in this node's (NIF-unit) space: how far a particle can get.
    const float r = static_cast<float>(max_life * (speed + speed_var / 2.0 + 0.5 * max_accel * max_life) +
                                       extent + (radius + radius_var / 2.0) * max_scale);
    particles->set_visibility_aabb(godot::AABB(Vector3(-r, -r, -r), Vector3(2 * r, 2 * r, 2 * r)));
    add_child(particles);
    em.particles = particles;
}

void SkydotParticles::set_birth_rate(double rate) {
    rate_ = rate;
    update_emission();
}

void SkydotParticles::set_active(bool active) {
    active_ = active;
    update_emission();
}

void SkydotParticles::update_emission() {
    for (Emitter& em : emitters_) {
        if (em.particles == nullptr) {
            continue;
        }
        const double rate = rate_ >= 0.0 ? rate_ : em.peak_rate;
        const bool on = active_ && rate > 0.0 && em.peak_rate > 0.0;
        em.particles->set_amount_ratio(
            em.peak_rate > 0.0 ? static_cast<float>(std::clamp(rate / em.peak_rate, 0.0, 1.0)) : 0.0f);
        if (em.particles->is_emitting() != on) {
            em.particles->set_emitting(on);
        }
    }
}

} // namespace skydot
