// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotAnimation`: skeletons and clips from animation assets (`.animfb`,
// formats/schema/animation.fbs), and skinned meshes moved onto them.
//
// A skeleton asset (`character assets/skeleton.hkx`) becomes a Skeleton3D in
// game units, Z-up, like the converter's GLBs below their
// `bethconv_z_up_to_y_up` node; put it under such a node. A clip asset
// becomes an Animation that drives that skeleton's bones: its splines are
// sampled once per frame here, at load, so playback costs what any Godot
// animation costs. Annotations ("FootLeft", "SoundPlay.…") become markers.
//
// Body parts are converted NIFs whose skins name the skeleton's bones;
// `attach_skinned` moves their meshes onto the actor's skeleton.
#pragma once

#include <godot_cpp/classes/animation.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace skydot {

class SkydotAnimation : public godot::RefCounted {
    GDCLASS(SkydotAnimation, godot::RefCounted)

public:
    /// What the asset holds: `skeletons` (names), `clips` (per clip:
    /// `duration`, `frames`, `tracks`, `skeleton`, `annotations`), or
    /// `error`.
    static godot::Dictionary describe(const godot::PackedByteArray& asset);

    /// The asset's first skeleton as a Skeleton3D at its reference pose, or
    /// null (with an error printed) if it has none.
    static godot::Skeleton3D* build_skeleton(const godot::PackedByteArray& asset);

    /// The asset's first clip as an Animation whose tracks are
    /// `<skeleton_path>:<bone>`. Track i drives the binding's bone, or bone i
    /// without one, of a skeleton built by `build_skeleton` from the file the
    /// clip was made for (indices, not names, as Havok binds them).
    /// Bones parked far away (prop slots the clip carries nothing for) are
    /// left out. Null on error.
    static godot::Ref<godot::Animation> build_clip(const godot::PackedByteArray& asset,
                                                   godot::Skeleton3D* skeleton,
                                                   const godot::String& skeleton_path);

    /// Move every skinned MeshInstance3D under `model` onto `skeleton`: binds
    /// are matched by bone name (case-insensitive) and recomputed against
    /// the skeleton's rest, so a mesh sits where its own skeleton put it.
    /// Returns how many meshes moved; binds naming a bone the skeleton lacks
    /// fall back to its root and are counted in `get_last_missing_bones`.
    static int attach_skinned(godot::Node* model, godot::Skeleton3D* skeleton);
    static int get_last_missing_bones();

protected:
    static void _bind_methods();
};

} // namespace skydot
