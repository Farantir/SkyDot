// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotBillboard`: turns its parent towards the camera every frame, as
// Gamebryo does for NiBillboardNode. The converter marks those nodes with a
// `billboard` mode in the node's glTF extras.
//
// Like NifSkope, the node's local frame is aligned with the viewer: +Z towards
// the camera, +Y up. The rotate-about-up modes turn only about the node's
// authored +Y axis.
#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <cstdint>

namespace skydot {

class SkydotBillboard : public godot::Node {
    GDCLASS(SkydotBillboard, godot::Node)

public:
    /// nifly BillboardMode values.
    enum Mode : std::int64_t {
        ALWAYS_FACE_CAMERA = 0,
        ROTATE_ABOUT_UP = 1,
        RIGID_FACE_CAMERA = 2,
        ALWAYS_FACE_CENTER = 3,
        RIGID_FACE_CENTER = 4,
        BS_ROTATE_ABOUT_UP = 5,
        ROTATE_ABOUT_UP2 = 9,
    };

    /// Give every node under `root` (inclusive) with a billboard mode in its
    /// extras a SkydotBillboard child. Returns how many were attached.
    static std::int64_t attach(godot::Node* root);

    void set_mode(std::int64_t mode);
    std::int64_t get_mode() const;

    /// Turn the parent towards the viewport's camera now; runs every frame.
    void face_camera();

    void _ready() override;
    void _process(double delta) override;

protected:
    static void _bind_methods();

private:
    std::int64_t mode_{ALWAYS_FACE_CAMERA};
    godot::Transform3D rest_;
};

} // namespace skydot
