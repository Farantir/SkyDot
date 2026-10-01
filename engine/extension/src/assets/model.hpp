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
#include <godot_cpp/variant/string.hpp>

namespace skydot {

class SkydotModel : public godot::Resource {
    GDCLASS(SkydotModel, godot::Resource)

public:
    ~SkydotModel() override;

    /// Take ownership of `root` (not in a tree) as the template.
    void set_template(godot::Node* root, const godot::String& vpath);

    /// A new copy of the model, owned by the caller. Its scene file path is
    /// the model's virtual path, which keys per-model caches.
    godot::Node* instantiate() const;

    godot::String get_vpath() const { return vpath_; }

protected:
    static void _bind_methods();

private:
    godot::Node* template_{nullptr};
    godot::String vpath_;
};

} // namespace skydot
