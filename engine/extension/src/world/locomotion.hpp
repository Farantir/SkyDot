// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which clips move an actor and how fast, found by name in its race's
// behaviour project without interpreting the behaviour graph (PLAN.md):
//
//   race behaviour   meshes/actors/character/defaultmale.hkx
//   project file     meshes/animationdata/defaultmale.txt      clip names ->
//                    clip id, playback speed; lists the behaviour graphs
//   behaviours       meshes/actors/character/behaviors/*.hkx   clip name ->
//                    animation file (hkbClipGenerator), relative to the
//                    project folder
//   root motion      meshes/animationdata/boundanims/anims_defaultmale.txt
//                    clip id -> distance over duration
//
// The DLC creatures' project and root motion are only in
// meshes/animationdatasinglefile.txt, read when a project has no file of its
// own. All are animation assets (formats/schema/animation.fbs). Clip names
// differ between creatures (MT_WalkForward, WalkForward00_Wolf, WalkF,
// H2HWalkForward); a gait takes the shortest name that says "walk forward"
// (or run) without a side, a pace (slow, fast) or a stance (combat, magic),
// and whose animation moves forward.
#pragma once

#include <godot_cpp/variant/packed_byte_array.hpp>

#include <functional>
#include <string>

namespace skydot {

struct GaitClip {
    std::string name;   ///< The project's clip name, for diagnostics.
    std::string file;   ///< The animation's virtual path (`.hkx`).
    float playback{1};  ///< The project's playback speed.
    /// Forward speed in game units per second at that playback speed; 0 for
    /// an idle or an animation without root motion.
    float speed{};

    [[nodiscard]] bool empty() const { return file.empty(); }
};

struct Locomotion {
    GaitClip idle;
    GaitClip walk;
    GaitClip run;
    std::string missing; ///< Why there is nothing (no project, no character), or empty.
};

/// `bytes(vpath)` returns an asset's bytes, empty if the pack lacks it;
/// `exists(vpath)` says whether it has it.
Locomotion find_locomotion(const std::string& behaviour,
                           const std::function<godot::PackedByteArray(const std::string&)>& bytes,
                           const std::function<bool(const std::string&)>& exists);

/// Lowercase, forward slashes, `.` and `..` resolved.
std::string normalize_path(const std::string& path);

} // namespace skydot
