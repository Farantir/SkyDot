// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotModel`: a mesh from the pack, built once and instanced by copying.
//
// The asset cache builds the node tree on a worker thread from the GLB
// (Godot's runtime glTF loader) and keeps it as a template outside the scene
// tree. `instantiate` duplicates it: meshes, materials and animations are
// shared, nothing is read back from the GPU. (`PackedScene::pack` would read
// mesh data back, which on a worker waits for the main thread.)
#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/texture.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace skydot {

class ModelCollision;

class SkydotModel : public godot::Resource {
    GDCLASS(SkydotModel, godot::Resource)

public:
    ~SkydotModel() override;

    /// Take ownership of `root` (not in a tree) as the template.
    void set_template(godot::Node* root, const godot::String& vpath);

    /// A new copy of the model, owned by the caller. Its scene file path is
    /// the model's virtual path, which keys per-model caches.
    godot::Node* instantiate() const;

    /// Give a copy made by `instantiate` its physics bodies (see
    /// world/collision.hpp). Returns how many; 0 if nothing collides.
    std::int64_t attach_collision(godot::Node* instance) const;
    /// Bodies each instance gets.
    std::int64_t get_body_count() const;

    godot::String get_vpath() const { return vpath_; }

    /// The model's physics bodies, or null if nothing collides.
    void set_collision(std::shared_ptr<const ModelCollision> collision) {
        collision_ = std::move(collision);
    }
    const std::shared_ptr<const ModelCollision>& collision() const { return collision_; }

    /// The textures its materials name, held while the model is: trimming
    /// the asset cache then keeps them for a model loaded ahead and not yet
    /// placed (materials are set up when it is).
    void set_textures(std::vector<godot::Ref<godot::Texture>> textures) { textures_ = std::move(textures); }

protected:
    static void _bind_methods();

private:
    godot::Node* template_{nullptr};
    godot::String vpath_;
    std::shared_ptr<const ModelCollision> collision_;
    std::vector<godot::Ref<godot::Texture>> textures_;
};

} // namespace skydot
