// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/water.hpp"
#include "render/materials.hpp"
#include "render/shader_source.hpp"

#include "skydot_formats/units.hpp"
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

Color unpack(std::uint32_t rgba) {
    return Color(static_cast<float>(rgba & 0xFFu) / 255.0F,
                 static_cast<float>((rgba >> 8) & 0xFFu) / 255.0F,
                 static_cast<float>((rgba >> 16) & 0xFFu) / 255.0F);
}

} // namespace

godot::Dictionary WaterMaterials::shader_codes() {
    godot::Dictionary out;
    out["water"] = shader_source::code("water.gdshader");
    return out;
}

Ref<godot::Shader> WaterMaterials::shader() {
    if (shader_.is_null()) {
        shader_ = shader_source::shader("water.gdshader");
    }
    return shader_;
}

void WaterMaterials::warm_up() {
    if (warm_.is_null()) {
        warm_.instantiate();
        warm_->set_shader(shader());
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
    // Water refracts the screen texture, which holds only opaque geometry, and
    // writes depth: drawn first among transparent surfaces, it neither paints
    // over the mist and spray above it (waterfall skirts) nor lets what lies
    // below show through on top of it.
    out->set_render_priority(godot::Material::RENDER_PRIORITY_MIN);
    if (water != nullptr) {
        const auto scale = static_cast<float>(formats::k_metres_per_unit);
        out->set_shader_parameter("shallow_color", shader_rgb(unpack(water->shallow_color())));
        out->set_shader_parameter("deep_color", shader_rgb(unpack(water->deep_color())));
        out->set_shader_parameter("reflection_color", shader_rgb(unpack(water->reflection_color())));
        out->set_shader_parameter("fresnel_amount", water->fresnel());
        out->set_shader_parameter("reflectivity", water->reflectivity());
        out->set_shader_parameter("sun_specular_power", water->sun_specular_power());
        out->set_shader_parameter("fog_near", water->fog_near() * scale);
        out->set_shader_parameter("fog_far", water->fog_far() * scale);
        out->set_shader_parameter("fog_amount", water->fog_amount());
        out->set_shader_parameter("refraction_magnitude", water->refraction_magnitude() * scale);
        out->set_shader_parameter("reflection_magnitude", water->reflection_magnitude());
        out->set_shader_parameter("sun_sparkle_magnitude", water->sun_sparkle_magnitude());
        out->set_shader_parameter("sun_specular_magnitude", water->sun_specular_magnitude());
        out->set_shader_parameter("sun_sparkle_power", water->sun_sparkle_power());
        out->set_shader_parameter("depth_control",
                                  Vector4(water->depth_reflections(), water->depth_refraction(),
                                          water->depth_normals(), water->depth_specular()));
        out->set_shader_parameter("noise_falloff", water->noise_falloff() * scale);
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
