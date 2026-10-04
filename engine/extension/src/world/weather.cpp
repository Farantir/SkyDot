// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/weather.hpp"

#include "world/materials.hpp"
#include "world/fb_search.hpp"

#include "assets/model.hpp"

#include "skydot_formats/flags.hpp"
#include "world_generated.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/particle_process_material.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/sky.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using godot::Color;
using godot::Dictionary;
using godot::Ref;
using godot::String;
using godot::Vector2;
using godot::Vector3;

namespace wfb = bethconv::pack::wfb;

namespace skydot {
namespace {

constexpr int k_cloud_layers = 29;
/// UV per real second at a stored speed of 1 (the byte at 254). A guess.
constexpr float k_cloud_scroll = 0.02F;
/// Sky objects sit just in front of the far plane (depth is reversed).
constexpr const char* k_far = R"(
void vertex() {
	POSITION = PROJECTION_MATRIX * MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
	POSITION.z = POSITION.w * 0.000001;
}
)";

constexpr const char* k_sky_shader = R"(
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
)";

/// Two weathers' textures for one layer, cross-faded by `mix_t`.
constexpr const char* k_cloud_shader = R"(
shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_disabled, fog_disabled, shadows_disabled;
uniform sampler2D tex_a : filter_linear_mipmap, repeat_enable;
uniform sampler2D tex_b : filter_linear_mipmap, repeat_enable;
uniform vec4 color_a = vec4(0.0);
uniform vec4 color_b = vec4(0.0);
uniform vec2 offset_a;
uniform vec2 offset_b;
uniform float mix_t = 1.0;
uniform vec3 flash_color;
uniform float flash = 0.0;
%FAR%
void fragment() {
	vec4 a = texture(tex_a, UV + offset_a) * color_a;
	vec4 b = texture(tex_b, UV + offset_b) * color_b;
	float wa = a.a * (1.0 - mix_t);
	float wb = b.a * mix_t;
	float alpha = wa + wb;
	vec3 rgb = alpha > 0.0001 ? (a.rgb * wa + b.rgb * wb) / alpha : vec3(0.0);
	// The dome's vertex colours are (1, 0, 0, fade): only the alpha means
	// anything.
	ALBEDO = rgb + flash_color * flash * 0.5;
	ALPHA = clamp(alpha * COLOR.a, 0.0, 1.0);
}
)";

/// Stars, the sun and its glare add light; the moons cover what is behind.
/// Every shape is in its texture's alpha: the constellation textures' colour
/// is a nebula under transparent texels, only the stars are opaque.
constexpr const char* k_sprite_shader = R"(
shader_type spatial;
render_mode unshaded, %BLEND%, depth_draw_never, cull_disabled, fog_disabled, shadows_disabled;
uniform sampler2D tex : filter_linear_mipmap;
uniform vec4 tint = vec4(1.0);
uniform bool use_alpha = true;
%FAR%
void fragment() {
	vec4 c = texture(tex, UV);
	ALBEDO = c.rgb * tint.rgb;
	ALPHA = clamp((use_alpha ? c.a : 1.0) * tint.a * COLOR.a, 0.0, 1.0);
}
)";

/// Rain and snow: camera-facing (rain turns about the vertical only), a
/// random frame of the atlas, the texture's alpha raised by `alpha_gain`
/// (the rain texture's peaks at 0.37).
constexpr const char* k_precipitation_shader = R"(
shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_disabled, shadows_disabled;
uniform sampler2D tex : filter_linear_mipmap;
uniform vec4 tint = vec4(1.0);
uniform float alpha_gain = 1.0;
uniform int frames_h = 1;
uniform int frames_v = 1;
uniform bool upright = false;
void vertex() {
	mat4 world = MODEL_MATRIX;
	vec3 scale = vec3(length(world[0].xyz), length(world[1].xyz), length(world[2].xyz));
	if (upright) {
		vec3 side = normalize(cross(vec3(0.0, 1.0, 0.0), INV_VIEW_MATRIX[2].xyz));
		vec3 back = cross(side, vec3(0.0, 1.0, 0.0));
		world = mat4(vec4(side * scale.x, 0.0), vec4(0.0, scale.y, 0.0, 0.0), vec4(back * scale.z, 0.0), world[3]);
	} else {
		float a = INSTANCE_CUSTOM.x;
		mat4 spin = mat4(vec4(cos(a), -sin(a), 0.0, 0.0), vec4(sin(a), cos(a), 0.0, 0.0),
		                 vec4(0.0, 0.0, 1.0, 0.0), vec4(0.0, 0.0, 0.0, 1.0));
		world = mat4(vec4(normalize(INV_VIEW_MATRIX[0].xyz) * scale.x, 0.0),
		             vec4(normalize(INV_VIEW_MATRIX[1].xyz) * scale.y, 0.0),
		             vec4(normalize(INV_VIEW_MATRIX[2].xyz) * scale.z, 0.0), world[3]) * spin;
	}
	MODELVIEW_MATRIX = VIEW_MATRIX * world;
	float h = float(max(frames_h, 1));
	float v = float(max(frames_v, 1));
	float frame = min(floor(INSTANCE_CUSTOM.z * h * v), h * v - 1.0);
	UV = UV / vec2(h, v) + vec2(mod(frame, h) / h, floor(frame / h) / v);
}
void fragment() {
	vec4 c = texture(tex, UV);
	ALBEDO = tint.rgb * c.rgb;
	ALPHA = clamp(c.a * alpha_gain * tint.a * COLOR.a, 0.0, 1.0);
}
)";

Ref<godot::Shader> make_shader(String code) {
    code = code.replace("%FAR%", k_far);
    Ref<godot::Shader> shader;
    shader.instantiate();
    shader->set_code(code);
    return shader;
}

Ref<godot::ShaderMaterial> material_from(const Ref<godot::Shader>& shader, int priority) {
    Ref<godot::ShaderMaterial> m;
    m.instantiate();
    m->set_shader(shader);
    m->set_render_priority(priority);
    return m;
}

Color unpack(std::uint32_t rgba) {
    return Color(static_cast<float>(rgba & 0xFFu) / 255.0F, static_cast<float>((rgba >> 8) & 0xFFu) / 255.0F,
                 static_cast<float>((rgba >> 16) & 0xFFu) / 255.0F);
}

String str(const flatbuffers::String* s) {
    return s == nullptr ? String() : String::utf8(s->c_str(), static_cast<int>(s->size()));
}

/// The texture named by slot 0 of a converted material's extras.
String slot0(const Ref<godot::Material>& material) {
    if (material.is_null() || !material->has_meta("extras")) {
        return {};
    }
    const godot::Variant extras = material->get_meta("extras");
    if (extras.get_type() != godot::Variant::DICTIONARY) {
        return {};
    }
    const godot::Variant block = Dictionary(extras).get("bethconv", godot::Variant());
    if (block.get_type() != godot::Variant::DICTIONARY) {
        return {};
    }
    const godot::Variant slots = Dictionary(block).get("texture_slots", godot::Variant());
    if (slots.get_type() != godot::Variant::DICTIONARY) {
        return {};
    }
    const godot::Variant entry = Dictionary(slots).get("0", godot::Variant());
    return entry.get_type() == godot::Variant::DICTIONARY ? String(Dictionary(entry).get("path", String()))
                                                          : String();
}

/// Even-odd test of (x, y) against a polygon of x, y pairs.
bool inside(const flatbuffers::Vector<float>* points, float x, float y) {
    if (points == nullptr || points->size() < 6) {
        return false;
    }
    const auto n = points->size() / 2;
    bool in = false;
    for (flatbuffers::uoffset_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = points->Get(i * 2);
        const float yi = points->Get(i * 2 + 1);
        const float xj = points->Get(j * 2);
        const float yj = points->Get(j * 2 + 1);
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
            in = !in;
        }
    }
    return in;
}

const char* const k_phases[8] = {"full",     "three_wan", "half_wan", "one_wan",
                                 "new",      "one_wax",   "half_wax", "three_wax"};

} // namespace

godot::Camera3D* SkydotWeather::camera() const {
    return godot::Object::cast_to<godot::Camera3D>(godot::ObjectDB::get_instance(camera_));
}

godot::Error SkydotWeather::setup(const Ref<SkydotWorld>& pack_world, std::int64_t world,
                                  godot::Camera3D* camera) {
    world_ = pack_world;
    world_id_ = static_cast<std::uint32_t>(world);
    camera_ = camera != nullptr ? godot::ObjectID(camera->get_instance_id()) : godot::ObjectID();
    climate_ = nullptr;
    if (world_.is_null() || world_->root_ == nullptr || world_->root_->climates() == nullptr) {
        return godot::ERR_UNCONFIGURED;
    }
    const auto climate_of = [&](std::uint32_t id) -> const wfb::Climate* {
        const auto* ws = world_->world_ptr(id);
        return ws != nullptr ? find_sorted(world_->root_->climates(), ws->climate(),
                                           [](const wfb::Climate* c) { return c->id(); })
                             : nullptr;
    };
    climate_ = climate_of(world_id_);
    if (climate_ == nullptr) {
        if (const auto* ws = world_->world_ptr(world_id_); ws != nullptr && ws->parent() != 0) {
            climate_ = climate_of(ws->parent());
        }
    }
    if (climate_ == nullptr) {
        return godot::ERR_DOES_NOT_EXIST;
    }
    build();
    if (to_ == 0) {
        std::uint32_t region = 0;
        const auto list = offered(region);
        region_ = region;
        set_weather(pick(list), 0.0);
    }
    std::uniform_real_distribution<double> hours(4.0, 10.0);
    hold_hours_ = hours(random_);
    return godot::OK;
}

void SkydotWeather::set_hour(double hour) { hour_ = std::fmod(std::fmod(hour, 24.0) + 24.0, 24.0); }

const SkydotWeather::Weather* SkydotWeather::weather_ptr(std::int64_t id) const {
    if (world_.is_null() || world_->root_ == nullptr || id == 0) {
        return nullptr;
    }
    return find_sorted(world_->root_->weathers(), static_cast<std::uint32_t>(id),
                       [](const Weather* w) { return w->id(); });
}

void SkydotWeather::set_weather(std::int64_t weather, double seconds) {
    const auto* target = weather_ptr(weather);
    if (target == nullptr) {
        return;
    }
    if (seconds < 0.0) {
        // DATA's transition delta; 10 / delta seconds (about 20 for most
        // vanilla weathers) is a guess.
        seconds = std::clamp(10.0 / std::max(static_cast<double>(target->transition_delta()), 0.05), 5.0, 120.0);
    }
    // A fade under way finishes at once: what shows becomes the old weather.
    from_ = to_ != 0 ? to_ : static_cast<std::uint32_t>(weather);
    to_ = static_cast<std::uint32_t>(weather);
    transition_ = seconds > 0.0 && from_ != to_ ? 0.0 : 1.0;
    transition_speed_ = seconds > 0.0 ? 1.0 / seconds : 1.0;
    for (auto& layer : layers_) {
        layer.offset[0] = layer.offset[1];
        layer.texture[0] = layer.texture[1];
    }
    if (const auto* clouds = target->clouds()) {
        for (std::size_t i = 0; i < layers_.size() && i < clouds->size(); ++i) {
            const auto* c = clouds->Get(static_cast<flatbuffers::uoffset_t>(i));
            auto& layer = layers_[i];
            const String path = str(c->texture());
            layer.texture[1] = c->enabled() ? path : String();
            if (layer.material.is_valid()) {
                layer.material->set_shader_parameter("tex_a", texture(layer.texture[0]));
                layer.material->set_shader_parameter("tex_b", texture(layer.texture[1]));
            }
        }
    }
    emit_signal("weather_changed", static_cast<std::int64_t>(to_));
}

std::vector<std::pair<std::uint32_t, std::int32_t>> SkydotWeather::offered(std::uint32_t& region) const {
    std::vector<std::pair<std::uint32_t, std::int32_t>> out;
    region = 0;
    auto* cam = camera();
    // The list is read once (the checks and the loop must see the same one).
    const auto* regions = world_->root_->regions();
    if (cam != nullptr && regions != nullptr) {
        const Vector3 at = SkydotWorld::godot_to_skyrim(cam->get_global_position());
        const auto* ws = world_->world_ptr(world_id_);
        const std::uint32_t parent = ws != nullptr ? ws->parent() : 0;
        int best = -1;
        for (const auto* r : *regions) {
            const auto* weathers = r->weathers();
            const auto* areas = r->areas();
            if ((r->world() != world_id_ && (parent == 0 || r->world() != parent)) ||
                weathers == nullptr || r->weather_priority() <= best || areas == nullptr) {
                continue;
            }
            for (const auto* area : *areas) {
                if (inside(area->points(), at.x, at.y)) {
                    best = r->weather_priority();
                    region = r->id();
                    out.clear();
                    for (const auto* w : *weathers) {
                        out.emplace_back(w->weather(), w->chance());
                    }
                    break;
                }
            }
        }
    }
    if (out.empty() && climate_ != nullptr && climate_->weathers() != nullptr) {
        for (const auto* w : *climate_->weathers()) {
            out.emplace_back(w->weather(), w->chance());
        }
    }
    return out;
}

std::uint32_t SkydotWeather::pick(const std::vector<std::pair<std::uint32_t, std::int32_t>>& list) {
    std::int64_t total = 0;
    for (const auto& [weather, chance] : list) {
        if (weather_ptr(weather) != nullptr) {
            total += std::max(chance, 0);
        }
    }
    if (total <= 0) {
        for (const auto& [weather, chance] : list) {
            if (weather_ptr(weather) != nullptr) {
                return weather;
            }
        }
        return 0;
    }
    std::uniform_int_distribution<std::int64_t> roll(0, total - 1);
    std::int64_t r = roll(random_);
    for (const auto& [weather, chance] : list) {
        if (weather_ptr(weather) == nullptr) {
            continue;
        }
        r -= std::max(chance, 0);
        if (r < 0) {
            return weather;
        }
    }
    return 0;
}

std::int64_t SkydotWeather::next_weather() {
    std::uint32_t region = 0;
    const auto chosen = pick(offered(region));
    region_ = region;
    if (chosen != 0) {
        set_weather(chosen);
    }
    return chosen;
}

void SkydotWeather::set_shadows(bool enabled) {
    shadows_ = enabled;
    if (light_ != nullptr) {
        light_->set_shadow(enabled);
    }
}

// ---- time of day ------------------------------------------------------------

void SkydotWeather::time_keys(int& from, int& to, float& t) const {
    float sun[4] = {5.5F, 10.0F, 16.0F, 20.5F};
    if (climate_ != nullptr) {
        sun[0] = climate_->sunrise_begin();
        sun[1] = climate_->sunrise_end();
        sun[2] = climate_->sunset_begin();
        sun[3] = climate_->sunset_end();
    }
    const auto h = static_cast<float>(hour_);
    struct Key {
        float hour;
        int time;
    };
    // Sunrise, day, sunset, night: keys at the start, middle and end of
    // sunrise and sunset, linear in between (as SkydotWorld.get_sky).
    const Key keys[] = {{sun[0], 3}, {(sun[0] + sun[1]) / 2, 0}, {sun[1], 1},
                        {sun[2], 1}, {(sun[2] + sun[3]) / 2, 2}, {sun[3], 3}};
    from = 3;
    to = 3;
    t = 0.0F;
    for (std::size_t i = 0; i + 1 < std::size(keys); ++i) {
        if (h >= keys[i].hour && h <= keys[i + 1].hour) {
            from = keys[i].time;
            to = keys[i + 1].time;
            const float span = keys[i + 1].hour - keys[i].hour;
            t = span > 0.0F ? (h - keys[i].hour) / span : 0.0F;
            return;
        }
    }
}

SkydotWeather::Sky SkydotWeather::sky_of(const Weather* weather) const {
    Sky out;
    const auto* colors = weather != nullptr ? weather->colors() : nullptr;
    if (colors == nullptr || colors->size() < 68) {
        return out;
    }
    int from = 0;
    int to = 0;
    float t = 0.0F;
    time_keys(from, to, t);
    const auto colour = [&](int index) {
        return unpack(colors->Get(static_cast<flatbuffers::uoffset_t>(index * 4 + from)))
            .lerp(unpack(colors->Get(static_cast<flatbuffers::uoffset_t>(index * 4 + to))), t);
    };
    out.upper = colour(0);
    out.fog_near_color = colour(1);
    out.ambient = colour(3);
    out.sunlight = colour(4);
    out.sun = colour(5);
    out.stars = colour(6);
    out.lower = colour(7);
    out.horizon = colour(8);
    out.fog_far_color = colour(12);
    const auto weight = [](int time) { return time == 1 ? 1.0F : time == 3 ? 0.0F : 0.5F; };
    const float daylight = weight(from) + (weight(to) - weight(from)) * t;
    if (const auto* fog = weather->fog(); fog != nullptr && fog->size() >= 8) {
        const auto mix = [&](flatbuffers::uoffset_t day, flatbuffers::uoffset_t night) {
            return fog->Get(night) + (fog->Get(day) - fog->Get(night)) * daylight;
        };
        out.fog_near = mix(0, 2);
        out.fog_far = mix(1, 3);
        out.fog_power = mix(4, 5);
        out.fog_max = mix(6, 7);
    }
    if (const auto* dalc = weather->directional_ambient(); dalc != nullptr && dalc->size() >= 28) {
        for (int side = 0; side < 6; ++side) {
            const auto at = [&](int time) {
                return unpack(dalc->Get(static_cast<flatbuffers::uoffset_t>(time * 7 + side)));
            };
            out.directional_ambient[static_cast<std::size_t>(side)] = at(from).lerp(at(to), t);
        }
        out.has_directional_ambient = true;
    }
    if (const auto* ids = weather->image_spaces(); ids != nullptr && ids->size() >= 4 &&
        world_->root_->image_spaces() != nullptr) {
        const auto values = [&](int time, std::array<float, 16>& into) {
            const auto* is = find_sorted(world_->root_->image_spaces(),
                                         ids->Get(static_cast<flatbuffers::uoffset_t>(time)),
                                         [](const bethconv::pack::wfb::ImageSpace* i) { return i->id(); });
            // Neutral where a part is missing: white 1, saturation,
            // brightness and contrast 1, no tint.
            into = {0, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1};
            if (is == nullptr) {
                return false;
            }
            const auto copy = [&](const flatbuffers::Vector<float>* v, std::size_t offset, std::size_t count) {
                if (v != nullptr && v->size() >= count) {
                    for (std::size_t i = 0; i < count; ++i) {
                        into[offset + i] = v->Get(static_cast<flatbuffers::uoffset_t>(i));
                    }
                }
            };
            copy(is->hdr(), 0, 9);
            copy(is->cinematic(), 9, 3);
            copy(is->tint(), 12, 4);
            return true;
        };
        std::array<float, 16> a{};
        std::array<float, 16> b{};
        const bool has_a = values(from, a);
        const bool has_b = values(to, b);
        if (has_a || has_b) {
            for (std::size_t i = 0; i < 16; ++i) {
                out.image_space[i] = a[i] + (b[i] - a[i]) * t;
            }
            out.has_image_space = true;
        }
    }
    return out;
}

godot::Dictionary SkydotWeather::get_image_space() const {
    const Sky a = sky_of(weather_ptr(from_));
    const Sky b = sky_of(weather_ptr(to_));
    if (!a.has_image_space && !b.has_image_space) {
        return {};
    }
    const auto t = static_cast<float>(transition_);
    const Sky& one = a.has_image_space ? a : b;
    const Sky& two = b.has_image_space ? b : a;
    godot::PackedFloat32Array hdr;
    godot::PackedFloat32Array cinematic;
    godot::PackedFloat32Array tint;
    for (std::size_t i = 0; i < 16; ++i) {
        const float v = one.image_space[i] + (two.image_space[i] - one.image_space[i]) * t;
        (i < 9 ? hdr : i < 12 ? cinematic : tint).push_back(v);
    }
    Dictionary out;
    out["hdr"] = hdr;
    out["cinematic"] = cinematic;
    out["tint"] = tint;
    return out;
}

Vector3 SkydotWeather::sun_direction(bool& day) const {
    float sun[4] = {5.5F, 10.0F, 16.0F, 20.5F};
    if (climate_ != nullptr) {
        sun[0] = climate_->sunrise_begin();
        sun[3] = climate_->sunset_end();
    }
    const auto h = static_cast<float>(hour_);
    const float pi = std::numbers::pi_v<float>;
    day = h >= sun[0] && h <= sun[3];
    float phase = 0.0F;
    if (day) {
        phase = (h - sun[0]) / std::max(sun[3] - sun[0], 0.1F);
    } else {
        const float night = 24.0F - (sun[3] - sun[0]);
        phase = std::fmod(h - sun[3] + 24.0F, 24.0F) / std::max(night, 0.1F);
    }
    // East (+X) to west through the south (+Z), as SkydotWorld.get_sky.
    const float azimuth = pi * phase;
    const float elevation = std::sin(pi * phase) * pi * 0.38F + 0.05F;
    return Vector3(std::cos(azimuth) * std::cos(elevation), std::sin(elevation),
                   std::sin(azimuth) * std::cos(elevation))
        .normalized();
}

// ---- building ---------------------------------------------------------------

Ref<godot::Texture2D> SkydotWeather::texture(const String& vpath) const {
    if (vpath.is_empty() || world_.is_null()) {
        return {};
    }
    return world_->resource(vpath);
}

void SkydotWeather::build() {
    environment_.instantiate();
    environment_->set_background(godot::Environment::BG_SKY);
    Ref<godot::Sky> sky;
    sky.instantiate();
    sky_material_.instantiate();
    sky_material_->set_shader(make_shader(k_sky_shader));
    sky->set_material(sky_material_);
    environment_->set_sky(sky);
    // SkydotImageSpace grades the gamma-space scene and hands over linear
    // colour; Godot only encodes it.
    environment_->set_tonemapper(godot::Environment::TONE_MAPPER_LINEAR);
    environment_->set_ambient_source(godot::Environment::AMBIENT_SOURCE_COLOR);
    environment_->set_fog_enabled(true);
    environment_->set_fog_mode(godot::Environment::FOG_MODE_DEPTH);
    environment_->set_fog_sky_affect(0.0F);
    auto* env_node = memnew(godot::WorldEnvironment);
    env_node->set_name("Environment");
    env_node->set_environment(environment_);
    add_child(env_node);

    light_ = memnew(godot::DirectionalLight3D);
    light_->set_name("Sun");
    light_->set_shadow(shadows_);
    add_child(light_);

    dome_ = memnew(godot::Node3D);
    dome_->set_name("Sky");
    add_child(dome_);
    build_clouds();
    build_sky_objects();

    particles_ = memnew(godot::GPUParticles3D);
    particles_->set_name("Precipitation");
    particles_->set_use_local_coordinates(false);
    particles_->set_emitting(false);
    add_child(particles_);
}

void SkydotWeather::build_clouds() {
    layers_.clear();
    const Ref<SkydotModel> model = world_->resource("meshes/sky/clouds.nif");
    auto* clouds = model.is_valid() ? godot::Object::cast_to<godot::Node3D>(model->instantiate()) : nullptr;
    if (clouds == nullptr) {
        godot::UtilityFunctions::push_warning("SkydotWeather: no meshes/sky/clouds.nif, so no clouds");
        return;
    }
    clouds->set_name("Clouds");
    dome_->add_child(clouds);
    const auto shader = make_shader(k_cloud_shader);
    // Shapes in file order; layer i is the i-th.
    const auto meshes = clouds->find_children("*", "MeshInstance3D", true, false);
    for (std::int64_t i = 0; i < meshes.size() && i < k_cloud_layers; ++i) {
        auto* mesh = godot::Object::cast_to<godot::MeshInstance3D>(meshes[i]);
        Layer layer;
        // Behind the stars, sun and moons it covers: drawn after them.
        layer.material = material_from(shader, -100 + static_cast<int>(i));
        mesh->set_material_override(layer.material);
        mesh->set_cast_shadows_setting(godot::GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
        layers_.push_back(layer);
    }
}

void SkydotWeather::build_sky_objects() {
    const auto additive = make_shader(String(k_sprite_shader).replace("%BLEND%", "blend_add"));
    const auto blended = make_shader(String(k_sprite_shader).replace("%BLEND%", "blend_mix"));

    // Stars: the climate's model, each shape with its own texture.
    const Ref<SkydotModel> stars = world_->resource(str(climate_->sky()));
    if (auto* node = stars.is_valid() ? godot::Object::cast_to<godot::Node3D>(stars->instantiate()) : nullptr) {
        node->set_name("Stars");
        dome_->add_child(node);
        for (const godot::Variant& v : node->find_children("*", "MeshInstance3D", true, false)) {
            auto* mesh = godot::Object::cast_to<godot::MeshInstance3D>(v);
            const Ref<godot::Mesh> geometry = mesh->get_mesh();
            auto m = material_from(additive, -128);
            const auto star_texture =
                geometry.is_valid() && geometry->get_surface_count() > 0
                    ? texture(slot0(geometry->surface_get_material(0)))
                    : Ref<godot::Texture2D>();
            if (star_texture.is_null()) {
                // Without its texture a star shape would light the whole sky.
                mesh->set_visible(false);
                continue;
            }
            m->set_shader_parameter("tex", star_texture);
            mesh->set_material_override(m);
            mesh->set_cast_shadows_setting(godot::GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
            stars_.push_back(m);
        }
    }

    const auto quad = [&](const char* name, const Ref<godot::ShaderMaterial>& material) {
        Ref<godot::QuadMesh> mesh;
        mesh.instantiate();
        mesh->set_size(Vector2(1, 1));
        auto* node = memnew(godot::MeshInstance3D);
        node->set_name(name);
        node->set_mesh(mesh);
        node->set_material_override(material);
        node->set_cast_shadows_setting(godot::GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
        dome_->add_child(node);
        return node;
    };
    sun_material_ = material_from(additive, -126);
    sun_material_->set_shader_parameter("tex", texture(str(climate_->sun_texture())));
    sun_ = quad("Sun", sun_material_);
    glare_material_ = material_from(additive, -125);
    glare_material_->set_shader_parameter("tex", texture(str(climate_->sun_glare_texture())));
    glare_ = quad("Glare", glare_material_);
    for (std::size_t i = 0; i < 2; ++i) {
        moon_materials_[i] = material_from(blended, -127);
        moons_[i] = quad(i == 0 ? "Masser" : "Secunda", moon_materials_[i]);
    }
}

// ---- per frame --------------------------------------------------------------

void SkydotWeather::_process(double delta) {
    if (world_.is_null() || climate_ == nullptr) {
        return;
    }
    const double game_seconds = delta * time_scale_;
    hour_ += game_seconds / 3600.0;
    while (hour_ >= 24.0) {
        hour_ -= 24.0;
        ++day_;
    }
    if (transition_ < 1.0) {
        transition_ = std::min(1.0, transition_ + delta * transition_speed_);
    }

    if (auto_weather_) {
        hold_hours_ -= game_seconds / 3600.0;
        region_check_ -= delta;
        if (region_check_ <= 0.0) {
            region_check_ = 2.0;
            std::uint32_t region = 0;
            const auto list = offered(region);
            if (region != region_) {
                region_ = region;
                const bool offered_here = std::ranges::any_of(list, [&](const auto& e) { return e.first == to_; });
                if (!offered_here) {
                    set_weather(pick(list));
                }
            }
        }
        if (hold_hours_ <= 0.0) {
            std::uniform_real_distribution<double> hours(4.0, 10.0);
            hold_hours_ = hours(random_);
            next_weather();
        }
    }

    if (auto* cam = camera()) {
        dome_->set_global_position(cam->get_global_position());
    }
    update_sky(delta);
    update_clouds(delta);
    update_sky_objects();
    update_precipitation();
}

void SkydotWeather::update_sky(double delta) {
    const auto* from = weather_ptr(from_);
    const auto* to = weather_ptr(to_);
    const Sky a = sky_of(from);
    const Sky b = sky_of(to);
    const auto t = static_cast<float>(transition_);
    const auto mix = [t](const Color& x, const Color& y) { return x.lerp(y, t); };

    // Lightning: rainy weathers whose thunder fades in during the
    // transition. Frequency 255 means none; lower is more often (a guess:
    // a flash every 4 to 44 seconds).
    double flash = 0.0;
    if (to != nullptr && formats::has_flag(to->classification(), wfb::WeatherClass::rainy) &&
        to->thunder_frequency() < 0.999F && transition_ >= static_cast<double>(to->thunder_begin())) {
        lightning_timer_ -= delta;
        if (lightning_timer_ <= 0.0) {
            std::exponential_distribution<double> wait(1.0 / (4.0 + 40.0 * static_cast<double>(to->thunder_frequency())));
            lightning_timer_ = wait(random_);
            since_flash_ = 0.0;
            emit_signal("lightning");
        }
    }
    if (since_flash_ >= 0.0) {
        since_flash_ += delta;
        flash = std::exp(-since_flash_ * 10.0);
    }
    const Color flash_color = to != nullptr ? unpack(to->lightning_color()) : Color(1, 1, 1);
    sky_material_->set_shader_parameter("upper", shader_rgb(mix(a.upper, b.upper)));
    sky_material_->set_shader_parameter("horizon", shader_rgb(mix(a.horizon, b.horizon)));
    sky_material_->set_shader_parameter("lower", shader_rgb(mix(a.lower, b.lower)));
    sky_material_->set_shader_parameter("flash", flash);
    sky_material_->set_shader_parameter("flash_color", shader_rgb(flash_color));
    for (auto& layer : layers_) {
        if (layer.material.is_valid()) {
            layer.material->set_shader_parameter("flash", flash);
            layer.material->set_shader_parameter("flash_color", shader_rgb(flash_color));
        }
    }

    const Color flash_light = flash_color * static_cast<float>(flash) * 0.5F;
    environment_->set_ambient_light_color(mix(a.ambient, b.ambient) + flash_light);
    // The game lights with the directional ambient (DALC), not NAM0's
    // ambient colour; that one stays for weathers without DALC.
    if (a.has_directional_ambient || b.has_directional_ambient) {
        godot::Array sides;
        for (std::size_t i = 0; i < 6; ++i) {
            const Color x = a.has_directional_ambient ? a.directional_ambient[i] : b.directional_ambient[i];
            const Color y = b.has_directional_ambient ? b.directional_ambient[i] : x;
            sides.push_back(mix(x, y) + flash_light);
        }
        environment_->set_meta("skydot_directional_ambient", sides);
    } else if (environment_->has_meta("skydot_directional_ambient")) {
        environment_->remove_meta("skydot_directional_ambient");
    }
    const float fog_near = a.fog_near + (b.fog_near - a.fog_near) * t;
    const float fog_far = a.fog_far + (b.fog_far - a.fog_far) * t;
    environment_->set_fog_depth_begin(static_cast<float>(static_cast<double>(fog_near) * SkydotWorld::UNIT_SCALE));
    environment_->set_fog_depth_end(
        static_cast<float>(static_cast<double>(std::max(fog_far, fog_near + 1.0F)) * SkydotWorld::UNIT_SCALE));
    environment_->set_fog_depth_curve(a.fog_power + (b.fog_power - a.fog_power) * t);
    environment_->set_fog_density(std::clamp(a.fog_max + (b.fog_max - a.fog_max) * t, 0.0F, 1.0F));
    environment_->set_fog_light_color(mix(a.fog_far_color, b.fog_far_color));
    // The near colour, for our shaders' fog (SkydotMaterials::sync_fog).
    environment_->set_meta("skydot_fog_near_color", mix(a.fog_near_color, b.fog_near_color));

    bool day = true;
    const Vector3 towards = sun_direction(day);
    // The image space's sunlight scale (HNAM) brightens the sun against
    // the ambient.
    const auto sun_scale = [](const Sky& s) { return s.has_image_space ? s.image_space[6] : 1.0F; };
    const float scale = sun_scale(a) + (sun_scale(b) - sun_scale(a)) * t;
    SkydotMaterials::set_game_light(light_, mix(a.sunlight, b.sunlight) * scale);
    environment_->set_meta("skydot_sun_direction", towards.normalized());
    environment_->set_meta("skydot_sun_color", shader_rgb(mix(a.sunlight, b.sunlight) * (day ? scale : 0.0F)));
    environment_->set_meta("skydot_sky_upper", shader_rgb(mix(a.upper, b.upper)));
    environment_->set_meta("skydot_sky_horizon", shader_rgb(mix(a.horizon, b.horizon)));
    light_->look_at_from_position(Vector3(), -towards, std::abs(towards.y) < 0.99F ? Vector3(0, 1, 0) : Vector3(0, 0, -1));
}

void SkydotWeather::update_clouds(double delta) {
    const std::array<const Weather*, 2> weathers{weather_ptr(from_), weather_ptr(to_)};
    int from = 0;
    int to = 0;
    float t = 0.0F;
    time_keys(from, to, t);
    for (std::size_t i = 0; i < layers_.size(); ++i) {
        auto& layer = layers_[i];
        for (std::size_t slot = 0; slot < 2; ++slot) {
            const auto* w = weathers[slot];
            const auto* clouds = w != nullptr ? w->clouds() : nullptr;
            const char* name = slot == 0 ? "color_a" : "color_b";
            if (clouds == nullptr || i >= clouds->size() || layer.texture[slot].is_empty()) {
                layer.material->set_shader_parameter(name, godot::Vector4());
                continue;
            }
            const auto* c = clouds->Get(static_cast<flatbuffers::uoffset_t>(i));
            layer.offset[slot] += Vector2(c->speed_x(), c->speed_y()) * k_cloud_scroll * static_cast<float>(delta);
            layer.offset[slot] = Vector2(std::fmod(layer.offset[slot].x, 1.0F), std::fmod(layer.offset[slot].y, 1.0F));
            Color colour(1, 1, 1, 1);
            float alpha = 1.0F;
            if (c->colors() != nullptr && c->colors()->size() >= 4) {
                colour = unpack(c->colors()->Get(static_cast<flatbuffers::uoffset_t>(from)))
                             .lerp(unpack(c->colors()->Get(static_cast<flatbuffers::uoffset_t>(to))), t);
            }
            if (c->alphas() != nullptr && c->alphas()->size() >= 4) {
                const float x = c->alphas()->Get(static_cast<flatbuffers::uoffset_t>(from));
                const float y = c->alphas()->Get(static_cast<flatbuffers::uoffset_t>(to));
                alpha = x + (y - x) * t;
            }
            colour.a = alpha;
            layer.material->set_shader_parameter(name, shader_rgba(colour));
            layer.material->set_shader_parameter(slot == 0 ? "offset_a" : "offset_b", layer.offset[slot]);
        }
        layer.material->set_shader_parameter("mix_t", static_cast<float>(transition_));
        layer.visible = !layer.texture[0].is_empty() || !layer.texture[1].is_empty();
    }
}

void SkydotWeather::update_sky_objects() {
    const auto* from = weather_ptr(from_);
    const auto* to = weather_ptr(to_);
    const Sky a = sky_of(from);
    const Sky b = sky_of(to);
    const auto t = static_cast<float>(transition_);
    bool day = true;
    const Vector3 towards = sun_direction(day);
    auto* cam = camera();
    if (cam == nullptr) {
        return;
    }
    // Sprites at an arbitrary distance: the shader puts them at the far
    // plane, so only their angular size matters.
    constexpr float k_distance = 10.0F;
    const auto place = [&](godot::MeshInstance3D* node, const Vector3& direction, float degrees) {
        const float size = 2.0F * k_distance * std::tan(degrees * std::numbers::pi_v<float> / 360.0F);
        const Vector3 at = direction.normalized() * k_distance;
        node->set_position(at);
        node->look_at(dome_->get_global_position() + at * 2.0F,
                      std::abs(direction.normalized().y) < 0.99F ? Vector3(0, 1, 0) : Vector3(0, 0, 1));
        node->set_scale(Vector3(size, size, size));
    };

    // The data gives full white at night and fades it with the time of day;
    // the square of the darkness keeps them out of the dusk sky longer (a
    // guess pending a comparison with the game).
    int from_key = 0;
    int to_key = 0;
    float key_t = 0.0F;
    time_keys(from_key, to_key, key_t);
    const auto weight = [](int time) { return time == 1 ? 1.0F : time == 3 ? 0.0F : 0.5F; };
    const float dark = 1.0F - (weight(from_key) + (weight(to_key) - weight(from_key)) * key_t);
    const Color stars = a.stars.lerp(b.stars, t) * (dark * dark);
    for (const auto& m : stars_) {
        m->set_shader_parameter("tint", shader_rgba(Color(stars.r, stars.g, stars.b, 1.0F)));
    }

    sun_->set_visible(day);
    glare_->set_visible(day);
    if (day) {
        place(sun_, towards, 4.0F);
        place(glare_, towards, 30.0F);
        sun_material_->set_shader_parameter("tint", shader_rgba(a.sun.lerp(b.sun, t)));
        const float glare_a = from != nullptr ? from->sun_glare() : 0.0F;
        const float glare_b = to != nullptr ? to->sun_glare() : 0.0F;
        Color glare = a.sun.lerp(b.sun, t);
        glare.a = glare_a + (glare_b - glare_a) * t;
        glare_material_->set_shader_parameter("tint", shader_rgba(glare));
    }

    // The moons: at night, near where the sun would be; their phase changes
    // every phase_length days.
    const int length = std::max<int>(climate_->phase_length(), 1);
    const int phase = static_cast<int>((day_ / length) % 8);
    const char* names[2] = {"masser", "secunda"};
    const float sizes[2] = {14.0F, 6.0F};
    const float shift[2] = {0.25F, -0.2F};
    for (std::size_t i = 0; i < 2; ++i) {
        const bool shown = !day && (climate_->moons() & (1U << i)) != 0;
        moons_[i]->set_visible(shown);
        if (!shown) {
            continue;
        }
        const Vector3 direction = (towards + Vector3(shift[i], 0.05F, -shift[i])).normalized();
        place(moons_[i], direction, sizes[i]);
        const String path = String("textures/sky/") + names[i] + "_" + k_phases[phase] + ".dds";
        if (String(moon_materials_[i]->get_meta("skydot_texture", String())) != path) {
            moon_materials_[i]->set_meta("skydot_texture", path);
            moon_materials_[i]->set_shader_parameter("tex", texture(path));
        }
    }
}

void SkydotWeather::configure_precipitation(const Weather* weather) {
    const std::uint32_t id = weather != nullptr ? weather->precipitation() : 0;
    if (id == particles_for_) {
        return;
    }
    particles_for_ = id;
    const auto* p = id != 0 ? find_sorted(world_->root_->precipitations(), id,
                                          [](const wfb::Precipitation* e) { return e->id(); })
                            : nullptr;
    if (p == nullptr) {
        particles_->set_emitting(false);
        return;
    }
    const auto s = static_cast<float>(SkydotWorld::UNIT_SCALE);
    const float box = static_cast<float>(p->box_size() != 0 ? p->box_size() : 1024) * s;
    const float speed = std::max(std::abs(p->gravity_velocity()), 1.0F) * s;
    const float density = p->density() > 0.0F ? p->density() : 1.0F;
    // Box size and density set the count; their units are not documented.
    const auto amount = static_cast<std::int32_t>(
        std::clamp(1600.0F * density * (box / (1000.0F * s)) * (box / (1000.0F * s)), 300.0F, 16000.0F));

    Ref<godot::ParticleProcessMaterial> process;
    process.instantiate();
    process->set_emission_shape(godot::ParticleProcessMaterial::EMISSION_SHAPE_BOX);
    process->set_emission_box_extents(Vector3(box / 2, box / 2, box / 2));
    process->set_direction(Vector3(0, -1, 0));
    process->set_spread(p->type() == 1 ? 15.0F : 2.0F);
    process->set_param_min(godot::ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, speed * 0.9F);
    process->set_param_max(godot::ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, speed * 1.1F);
    process->set_gravity(Vector3(0, 0, 0));
    process->set_param_min(godot::ParticleProcessMaterial::PARAM_ANGLE, 0.0F);
    process->set_param_max(godot::ParticleProcessMaterial::PARAM_ANGLE, p->rotation_range());
    process->set_param_min(godot::ParticleProcessMaterial::PARAM_ANGULAR_VELOCITY, -p->rotation_velocity());
    process->set_param_max(godot::ParticleProcessMaterial::PARAM_ANGULAR_VELOCITY, p->rotation_velocity());
    process->set_param_min(godot::ParticleProcessMaterial::PARAM_ANIM_OFFSET, 0.0F);
    process->set_param_max(godot::ParticleProcessMaterial::PARAM_ANIM_OFFSET, 1.0F);
    if (p->type() == 1) {
        process->set_turbulence_enabled(true);
        process->set_turbulence_noise_strength(0.5F);
    }
    // Wind drifts it.
    if (weather != nullptr) {
        const float angle = weather->wind_direction() * std::numbers::pi_v<float> / 180.0F;
        const Vector3 wind = SkydotWorld::skyrim_position(Vector3(std::sin(angle), std::cos(angle), 0)).normalized() *
                             weather->wind_speed() * 4.0F;
        process->set_gravity(wind);
    }

    // Particle size units are not documented. Snow at ten game units per
    // unit gives flakes of a few centimetres; the rain texture is a
    // one-pixel streak in each eight-pixel frame, so rain needs four times
    // that (streaks about 1.1 m long) to show at all. Both are guesses.
    Ref<godot::QuadMesh> quad;
    quad.instantiate();
    quad->set_size(Vector2(p->size_x(), p->size_y()) * (p->type() == 0 ? 40.0F : 10.0F) * s);
    auto material = material_from(make_shader(k_precipitation_shader), 0);
    material->set_shader_parameter("tex", texture(str(p->texture())));
    material->set_shader_parameter("frames_h", static_cast<std::int32_t>(std::max<std::uint32_t>(p->subtextures_x(), 1)));
    material->set_shader_parameter("frames_v", static_cast<std::int32_t>(std::max<std::uint32_t>(p->subtextures_y(), 1)));
    material->set_shader_parameter("upright", p->type() == 0);
    material->set_shader_parameter("alpha_gain", p->type() == 0 ? 2.5F : 1.0F);
    quad->set_material(material);

    particles_->set_amount(amount);
    particles_->set_lifetime(static_cast<double>(box / speed));
    // Start with the box full rather than with the first drops at its top.
    particles_->set_pre_process_time(static_cast<double>(box / speed));
    particles_->set_process_material(process);
    particles_->set_draw_pass_mesh(0, quad);
    particles_->set_visibility_aabb(godot::AABB(Vector3(-box, -box * 2, -box), Vector3(box * 2, box * 4, box * 2)));
    particles_->set_emitting(true);
}

void SkydotWeather::update_precipitation() {
    const auto* from = weather_ptr(from_);
    const auto* to = weather_ptr(to_);
    const auto t = static_cast<float>(transition_);
    // The old weather's precipitation fades out by its end point, the new
    // one's in from its begin point.
    float out = 0.0F;
    float in = 0.0F;
    if (from != nullptr && from->precipitation() != 0 && from_ != to_) {
        out = 1.0F - std::clamp(t / std::max(from->precipitation_end(), 0.05F), 0.0F, 1.0F);
    }
    if (to != nullptr && to->precipitation() != 0) {
        const float begin = to->precipitation_begin();
        in = begin >= 0.999F ? (t >= 1.0F ? 1.0F : 0.0F) : std::clamp((t - begin) / (1.0F - begin), 0.0F, 1.0F);
    }
    const auto* shown = in > 0.0F || out <= 0.0F ? to : from;
    precipitation_ = static_cast<double>(in > 0.0F ? in : out);
    configure_precipitation(precipitation_ > 0.0 ? shown : nullptr);
    if (particles_for_ == 0) {
        return;
    }
    particles_->set_amount_ratio(static_cast<float>(precipitation_));
    if (auto* cam = camera()) {
        particles_->set_global_position(cam->get_global_position() + Vector3(0, 2, 0));
    }
    // Lit by the sky: ambient plus some sunlight.
    const Sky a = sky_of(from);
    const Sky b = sky_of(to);
    const Color light = (a.ambient.lerp(b.ambient, t) + a.sunlight.lerp(b.sunlight, t) * 0.3F).clamp();
    const Ref<godot::QuadMesh> quad = particles_->get_draw_pass_mesh(0);
    if (quad.is_valid()) {
        const Ref<godot::ShaderMaterial> material = quad->get_material();
        if (material.is_valid()) {
            material->set_shader_parameter("tint", shader_rgba(Color(light.r, light.g, light.b, 0.8F)));
        }
    }
}

godot::Dictionary SkydotWeather::get_state() const {
    Dictionary out;
    out["weather"] = static_cast<std::int64_t>(to_);
    out["previous"] = static_cast<std::int64_t>(from_);
    out["transition"] = transition_;
    out["hour"] = hour_;
    out["day"] = day_;
    int from = 0;
    int to = 0;
    float t = 0.0F;
    time_keys(from, to, t);
    const auto weight = [](int time) { return time == 1 ? 1.0F : time == 3 ? 0.0F : 0.5F; };
    out["daylight"] = weight(from) + (weight(to) - weight(from)) * t;
    out["region"] = static_cast<std::int64_t>(region_);
    out["precipitation"] = precipitation_;
    out["lightning"] = since_flash_;
    std::int64_t visible = 0;
    for (const auto& layer : layers_) {
        visible += layer.visible ? 1 : 0;
    }
    out["clouds"] = visible;
    const auto* w = weather_ptr(to_);
    out["editor_id"] = w != nullptr ? str(w->editor_id()) : String();
    return out;
}

void SkydotWeather::_bind_methods() {
    using godot::D_METHOD;
    godot::ClassDB::bind_method(D_METHOD("setup", "world", "world_id", "camera"), &SkydotWeather::setup);
    godot::ClassDB::bind_method(D_METHOD("set_hour", "hour"), &SkydotWeather::set_hour);
    godot::ClassDB::bind_method(D_METHOD("get_hour"), &SkydotWeather::get_hour);
    godot::ClassDB::bind_method(D_METHOD("set_day", "day"), &SkydotWeather::set_day);
    godot::ClassDB::bind_method(D_METHOD("get_day"), &SkydotWeather::get_day);
    godot::ClassDB::bind_method(D_METHOD("set_time_scale", "scale"), &SkydotWeather::set_time_scale);
    godot::ClassDB::bind_method(D_METHOD("get_time_scale"), &SkydotWeather::get_time_scale);
    godot::ClassDB::bind_method(D_METHOD("set_weather", "weather", "seconds"), &SkydotWeather::set_weather,
                                DEFVAL(-1.0));
    godot::ClassDB::bind_method(D_METHOD("get_weather"), &SkydotWeather::get_weather);
    godot::ClassDB::bind_method(D_METHOD("get_previous_weather"), &SkydotWeather::get_previous_weather);
    godot::ClassDB::bind_method(D_METHOD("set_auto_weather", "enabled"), &SkydotWeather::set_auto_weather);
    godot::ClassDB::bind_method(D_METHOD("get_auto_weather"), &SkydotWeather::get_auto_weather);
    godot::ClassDB::bind_method(D_METHOD("next_weather"), &SkydotWeather::next_weather);
    godot::ClassDB::bind_method(D_METHOD("set_shadows", "enabled"), &SkydotWeather::set_shadows);
    godot::ClassDB::bind_method(D_METHOD("get_shadows"), &SkydotWeather::get_shadows);
    godot::ClassDB::bind_method(D_METHOD("get_state"), &SkydotWeather::get_state);
    godot::ClassDB::bind_method(D_METHOD("get_image_space"), &SkydotWeather::get_image_space);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::FLOAT, "hour"), "set_hour", "get_hour");
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::INT, "day"), "set_day", "get_day");
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::FLOAT, "time_scale"), "set_time_scale", "get_time_scale");
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::BOOL, "auto_weather"), "set_auto_weather", "get_auto_weather");
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::BOOL, "shadows"), "set_shadows", "get_shadows");
    ADD_SIGNAL(godot::MethodInfo("weather_changed", godot::PropertyInfo(godot::Variant::INT, "weather")));
    ADD_SIGNAL(godot::MethodInfo("lightning"));
}

} // namespace skydot
