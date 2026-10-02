// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/water.hpp"
#include "world/materials.hpp"

#include "world/world.hpp"

#include "world_generated.h"

#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <cmath>
#include <numbers>

using godot::Color;
using godot::Ref;
using godot::String;
using godot::Vector4;

namespace wfb = bethconv::pack::wfb;

namespace skydot {

namespace {

constexpr const char* k_water_shader = R"(shader_type spatial;
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never;

uniform vec3 shallow_color : source_color = vec3(0.15, 0.2, 0.15);
uniform vec3 deep_color : source_color = vec3(0.02, 0.06, 0.07);
uniform vec3 reflection_color : source_color = vec3(0.45, 0.52, 0.6);
uniform float opacity = 0.3;
uniform float fresnel_amount = 0.05;
uniform float reflectivity = 1.0;
uniform float refraction = 0.02;
// Metres of water over which shallow turns deep.
uniform float fog_far = 1.6;
uniform sampler2D noise_tex : hint_normal, filter_linear_mipmap, repeat_enable;
// Per layer: metres per tile, direction (radians), speed (tiles per second),
// amplitude.
uniform vec4 layer_0 = vec4(27.0, 0.0, 0.02, 0.7);
uniform vec4 layer_1 = vec4(96.0, 0.0, 0.01, 0.6);
uniform vec4 layer_2 = vec4(7.0, 0.0, 0.1, 0.5);
uniform sampler2D depth_tex : hint_depth_texture;
uniform sampler2D screen_tex : hint_screen_texture, filter_linear_mipmap;

varying vec3 world_pos;

void vertex() {
	world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

vec2 ripple(vec4 layer) {
	// Skyrim's north is Godot's -Z.
	vec2 wind = vec2(sin(layer.y), -cos(layer.y)) * layer.z * TIME;
	vec2 n = texture(noise_tex, world_pos.xz / max(layer.x, 0.01) + wind).xy * 2.0 - 1.0;
	return n * layer.w;
}

void fragment() {
	vec2 n = ripple(layer_0) + ripple(layer_1) + ripple(layer_2);
	vec3 world_normal = normalize(vec3(n.x, 4.0, n.y));
	vec3 normal = normalize((VIEW_MATRIX * vec4(world_normal, 0.0)).xyz);

	// Water depth under this point along the view ray.
	float depth = texture(depth_tex, SCREEN_UV).r;
	vec4 behind = INV_PROJECTION_MATRIX * vec4(SCREEN_UV * 2.0 - 1.0, depth, 1.0);
	float thickness = max(VERTEX.z - behind.z / behind.w, 0.0);
	float deep = clamp(thickness / max(fog_far, 0.01), 0.0, 1.0);

	vec3 below = textureLod(screen_tex, SCREEN_UV + n * refraction * deep, 0.0).rgb;
	vec3 color = mix(below, shallow_color, opacity);
	color = mix(color, deep_color, deep);
	float fresnel = fresnel_amount +
			(1.0 - fresnel_amount) * pow(1.0 - clamp(dot(normal, VIEW), 0.0, 1.0), 5.0);
	color = mix(color, reflection_color, clamp(fresnel * reflectivity, 0.0, 1.0));

	ALBEDO = color;
	// Fade in over the first 10 cm, so shorelines do not show a hard edge.
	ALPHA = clamp(thickness / 0.1, 0.0, 1.0);
}
)";

Color unpack(std::uint32_t rgba) {
    return Color(static_cast<float>(rgba & 0xFFu) / 255.0F,
                 static_cast<float>((rgba >> 8) & 0xFFu) / 255.0F,
                 static_cast<float>((rgba >> 16) & 0xFFu) / 255.0F);
}

} // namespace

Ref<godot::Shader> WaterMaterials::shader() {
    if (shader_.is_null()) {
        shader_.instantiate();
        shader_->set_code(godot::String::utf8(with_game_fog(k_water_shader).c_str()));
    }
    return shader_;
}

void WaterMaterials::warm_up() {
    if (warm_.is_null()) {
        warm_.instantiate();
        warm_->set_shader(shader());
        warm_->set_shader_parameter("opacity", 0.3);
    }
}

Ref<godot::ShaderMaterial> WaterMaterials::material(const wfb::Water* water,
                                                    const TextureLoader& load) {
    const std::uint32_t id = water != nullptr ? water->id() : 0;
    if (auto it = materials_.find(id); it != materials_.end()) {
        return it->second;
    }
    Ref<godot::ShaderMaterial> out;
    out.instantiate();
    out->set_shader(shader());
    if (water != nullptr) {
        const auto scale = static_cast<float>(SkydotWorld::UNIT_SCALE);
        out->set_shader_parameter("shallow_color", unpack(water->shallow_color()));
        out->set_shader_parameter("deep_color", unpack(water->deep_color()));
        out->set_shader_parameter("reflection_color", unpack(water->reflection_color()));
        out->set_shader_parameter("opacity", static_cast<float>(water->opacity()) / 100.0F);
        out->set_shader_parameter("fresnel_amount", water->fresnel());
        out->set_shader_parameter("reflectivity", water->reflectivity());
        if (water->fog_far() > water->fog_near()) {
            out->set_shader_parameter("fog_far", water->fog_far() * scale);
        }
        if (const auto* layers = water->layers()) {
            for (flatbuffers::uoffset_t i = 0; i < layers->size() && i < 3; ++i) {
                const auto* l = layers->Get(i);
                const float direction = l->wind_direction() * std::numbers::pi_v<float> / 180.0F;
                out->set_shader_parameter(String("layer_") + String::num_int64(i),
                                          Vector4(l->uv_scale() * scale, direction,
                                                  l->wind_speed(), l->amplitude()));
            }
        }
        if (const auto* noise = water->noise(); noise != nullptr && noise->size() != 0) {
            if (Ref<godot::Texture> texture = load(noise->Get(0)->str()); texture.is_valid()) {
                out->set_shader_parameter("noise_tex", texture);
            }
        }
    }
    materials_.emplace(id, out);
    return out;
}

} // namespace skydot
