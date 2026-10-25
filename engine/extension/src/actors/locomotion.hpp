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
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skydot {

struct GaitClip {
    std::string name;   ///< The project's clip name, for diagnostics.
    std::string file;   ///< The animation's virtual path (`.hkx`).
    float playback{1};  ///< The project's playback speed.
    /// Forward speed in game units per second at that playback speed; 0 for
    /// an idle or an animation without root motion.
    float speed{};
    /// The project's text keys of the clip (HitFrame, SoundPlay.*): time in seconds, text.
    std::vector<std::pair<float, std::string>> notes;

    [[nodiscard]] bool empty() const { return file.empty(); }
};

struct Locomotion {
    GaitClip idle;
    GaitClip walk;
    GaitClip run;
    /// Turning on the spot (the dragon's mtturnleftground45...), empty where the project has none.
    GaitClip turn_left;
    GaitClip turn_right;
    /// The clip a death plays instead of a ragdoll: the project's clip whose file ends in
    /// "deathintro.hkx" (the dragon's mtdeathintro); empty for a race that falls apart.
    GaitClip death;
    /// Every clip of the project that has an animation in the pack, by lower-case name without
    /// ".hkx" (`set_stance` finds the combat set here).
    std::unordered_map<std::string, GaitClip> clips;
    std::string missing; ///< Why there is nothing (no project, no character), or empty.
};

/// `bytes(vpath)` returns an asset's bytes, empty if the pack lacks it;
/// `exists(vpath)` says whether it has it.
Locomotion find_locomotion(const std::string& behaviour,
                           const std::function<godot::PackedByteArray(const std::string&)>& bytes,
                           const std::function<bool(const std::string&)>& exists);

/// The idle, walk and run of a stance (`1hm`, `2hm`, `bow`, `combat`...): each empty where the
/// project has no clip of that name (the actor keeps its normal one then).
struct Stance {
    GaitClip idle;
    GaitClip walk;
    GaitClip run;
    [[nodiscard]] bool empty() const { return idle.empty() && walk.empty() && run.empty(); }
};

/// The stance `prefix` (any case) of `locomotion`'s project, found by name.
Stance find_stance(const Locomotion& locomotion, std::string_view prefix);

/// Lowercase, forward slashes, `.` and `..` resolved.
std::string normalize_path(const std::string& path);

} // namespace skydot
