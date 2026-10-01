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

#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace skydot {

struct EffectAsset;

class SkydotMaterials : public godot::RefCounted {
    GDCLASS(SkydotMaterials, godot::RefCounted)

public:
    /// Convert every surface material under `root` (inclusive). Returns the
    /// number of surfaces given a converted material.
    std::int64_t apply(godot::Node* root);

    /// The converted material for one source material, or null if it carries
    /// no `bethconv` extras.
    godot::Ref<godot::Material> convert(const godot::Ref<godot::Material>& source);

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

    /// Converted materials and shader variants so far.
    std::int64_t get_material_count() const;
    std::int64_t get_shader_count() const;

protected:
    static void _bind_methods();

private:
    godot::Ref<godot::Shader> shader_for(const std::string& code);
    void configure_effect(const godot::Ref<godot::ShaderMaterial>& out,
                          const godot::Dictionary& extras, bool double_sided, bool particles,
                          const godot::Ref<godot::Texture>& source, godot::Color emission);
    godot::Ref<godot::Texture> load_texture(const godot::String& vpath);

    std::unordered_map<std::uint64_t, godot::Ref<godot::Material>> materials_;
    std::unordered_map<std::string, godot::Ref<godot::Shader>> shaders_;
    std::vector<godot::Ref<godot::ShaderMaterial>> warm_;
    std::unordered_map<std::string, godot::Ref<godot::Texture>> textures_;
    std::unordered_map<std::string, std::shared_ptr<const EffectAsset>> effects_;
    std::unordered_map<std::string, godot::Ref<godot::ShaderMaterial>> particle_materials_;
};

} // namespace skydot
