// SPDX-License-Identifier: GPL-3.0-or-later
//
// Water surfaces from world.fb's `Water` (WATR): three noise layers scrolling
// with their wind direction and speed, a shallow-to-deep colour by the depth
// of water under the surface (over WATR's fog distance), refraction of what
// lies below, and a Fresnel blend towards the reflection colour. Not the
// game's shader: no sky or scene reflection, no sun sparkle, no underwater
// view.
#pragma once

#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace bethconv::pack::wfb {
struct Water;
} // namespace bethconv::pack::wfb

namespace skydot {

class WaterMaterials {
public:
    /// Loads a texture by virtual path; null if missing.
    using TextureLoader = std::function<godot::Ref<godot::Texture>(const std::string& vpath)>;

    /// The material for a water type; `water` may be null (a default look).
    godot::Ref<godot::ShaderMaterial> material(const bethconv::pack::wfb::Water* water,
                                               const TextureLoader& load);

    /// Create the shader and a material now (see SkydotMaterials::warm_up).
    void warm_up();

private:
    godot::Ref<godot::Shader> shader();

    godot::Ref<godot::Shader> shader_;
    godot::Ref<godot::ShaderMaterial> warm_;
    std::unordered_map<std::uint32_t, godot::Ref<godot::ShaderMaterial>> materials_;
};

} // namespace skydot
