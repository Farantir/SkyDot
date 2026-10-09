// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotMaterials`: replaces the StandardMaterial3D that Godot's glTF importer
// makes for each converted mesh with a ShaderMaterial that follows Skyrim's own
// shading, driven by the `bethconv` block the converter writes into the
// material's glTF extras (kept by Godot as the material's "extras" meta).
//
// Three families:
//   - lighting (BSLightingShaderProperty): albedo, normal map whose alpha is the
//     specular mask, Blinn-Phong specular from glossiness/strength/color, glow
//     map, environment map, vertex colors, alpha test or blend;
//   - effect (BSEffectShaderProperty): unshaded, greyscale-to-palette color and
//     alpha, blend mode from the NiAlphaProperty flags;
//   - refraction (lighting with SLSF1_REFRACTION): screen-space distortion.
//
// Converted materials are cached by source material, so every instance of a
// mesh shares them.
#pragma once

#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/light3d.hpp>

#include "assets/asset_cache.hpp"

#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace skydot {

struct EffectAsset;

/// A colour for a shader uniform, as the game's numbers: Godot sRGB-decodes
/// Color values for every vec3/vec4 uniform, hint or not, so colours go to
/// our shaders as vectors.
inline godot::Vector4 shader_rgba(const godot::Color& c) { return {c.r, c.g, c.b, c.a}; }
inline godot::Vector3 shader_rgb(const godot::Color& c) { return {c.r, c.g, c.b}; }

// Rendering happens in the game's gamma space, as Skyrim does: textures
// (no source_color), vertex colours, light, ambient and fog colours are the
// game's numbers, lighting adds them up as the game does, and the image
// space pass (SkydotImageSpace) turns the result into display colour. Godot
// converts the colours of lights and environments from sRGB; these undo it.

/// A directional material (MATO) as the lighting shader takes it.
struct ProjectedMaterial {
    godot::Ref<godot::Texture> albedo;   ///< null: one colour
    godot::Vector4 params;               ///< falloff scale, falloff bias, noise and texture repeats per metre
    godot::Vector3 direction{0, 1, 0};   ///< faces turned this way (Godot's world axes) take it
    godot::Vector3 color{1, 1, 1};
    float normal_dampener = 0.0F;
    /// Single pass materials: the engine's projected snow textures (all four,
    /// or the snow is `color` flat) and the noise's size in game units.
    godot::Ref<godot::Texture> snow_noise, snow_diffuse, snow_normal, snow_detail;
    float noise_units = 50.0F;
};

class SkydotMaterials : public godot::RefCounted {
    GDCLASS(SkydotMaterials, godot::RefCounted)

public:
    /// Where textures come from. Without it materials are untextured.
    void set_assets(std::shared_ptr<AssetCache> assets) { assets_ = std::move(assets); }

    /// Convert every surface material under `root` (inclusive). Returns the
    /// number of surfaces given a converted material.
    std::int64_t apply(godot::Node* root);

    /// The converted material for one source material, or null if it carries
    /// no `bethconv` extras.
    godot::Ref<godot::Material> convert(const godot::Ref<godot::Material>& source);

    /// `material` (converted) with directional material `with` (cached under
    /// `key`) if its shape has the Projected UV flag; else `material`.
    godot::Ref<godot::ShaderMaterial> projected(const godot::Ref<godot::ShaderMaterial>& material,
                                                std::uint32_t key, const ProjectedMaterial& with);

    /// The camera-facing effect material for a particle system, from the
    /// material block the converter writes with it.
    godot::Ref<godot::ShaderMaterial> particle_material(const godot::Dictionary& extras);

    /// The effect extras of an instantiated model, parsed once per scene file.
    std::shared_ptr<const EffectAsset> effects(godot::Node* root);

    /// particle_material for particle system `index` of the model at
    /// `scene`, shared by every instance of it.
    godot::Ref<godot::ShaderMaterial> particle_material_for(const godot::String& scene,
                                                            std::int64_t index,
                                                            const godot::Dictionary& block);

    /// The process shader of every particle system.
    godot::Ref<godot::Shader> particles_process_shader();

    /// Create every shader variant now, so no conversion compiles one later
    /// (a first-time compile stalls the frame for tens of milliseconds).
    /// Returns the number of variants.
    std::int64_t warm_up();

    /// The code of every shader the engine can produce (material variants,
    /// terrain, water, LOD, weather, particles, image space), by name.
    static godot::Dictionary shader_sources();

    /// Converted materials and shader variants so far.
    std::int64_t get_material_count() const;
    std::int64_t get_shader_count() const;

protected:
    static void _bind_methods();

public:
    /// Copy `environment`'s depth fog into the global shader parameters our
    /// shaders compute the game's fog from (game_fog.gdshaderinc): begin, end,
    /// curve (power) and density (max), the far colour from its fog light
    /// colour and the near colour from its "skydot_fog_near_color" meta.
    /// Call when the fog changes, or once a frame.
    static void sync_fog(const godot::Ref<godot::Environment>& environment);
    /// Also copies the environment's "skydot_directional_ambient" meta (six
    /// Colors in the game's gamma space, x+, x-, y+, y-, z+, z- in the game's
    /// axes) into the ambient our shaders use, or, without it, its ambient
    /// light colour from every side.
    /// Register those parameters (no fog) if they are not yet.
    static void ensure_fog_globals();
    /// Mip levels added to the albedo lookups of foliage materials (the
    /// "foliage" uniform of lighting.gdshaderinc), for all of them at once.
    static void set_foliage_bias(float bias);
    /// Set `light` so that shaders see `gamma` (a game colour, any
    /// brightness) as its light: Godot's colour and energy.
    static void set_game_light(godot::Light3D* light, const godot::Color& gamma);
    /// The colour Godot turns into `gamma` (for Environment colours, at most 1).
    static godot::Color game_color(const godot::Color& gamma);

private:
    godot::Ref<godot::Shader> shader_for(const std::string& code);
    void configure_effect(const godot::Ref<godot::ShaderMaterial>& out,
                          const godot::Dictionary& extras, bool double_sided, bool particles,
                          const godot::Ref<godot::Texture>& source, godot::Color emission);
    godot::Ref<godot::Texture> load_texture(const godot::String& vpath);

    std::shared_ptr<AssetCache> assets_;
    std::mutex projected_mutex_;
    std::map<std::pair<std::uint64_t, std::uint32_t>, godot::Ref<godot::ShaderMaterial>> projected_;
    std::unordered_map<std::uint64_t, godot::Ref<godot::Material>> materials_;
    std::unordered_map<std::string, godot::Ref<godot::Shader>> shaders_;
    std::vector<godot::Ref<godot::ShaderMaterial>> warm_;
    std::unordered_map<std::string, godot::Ref<godot::Texture>> textures_;
    std::unordered_map<std::string, std::shared_ptr<const EffectAsset>> effects_;
    std::unordered_map<std::string, godot::Ref<godot::ShaderMaterial>> particle_materials_;
};

} // namespace skydot
