// SPDX-License-Identifier: GPL-3.0-or-later
//
// A behaviour project's animationdata: the files it lists, its clips (by name,
// with the id the motions are keyed by) and the root motion of each clip,
// read from the pack's animation assets (formats/schema/animation.fbs). One
// reader for locomotion's speeds, the actor graphs' clip files and root
// motion. No Godot in here.
//
//   race behaviour   meshes/actors/character/defaultmale.hkx
//   project file     meshes/animationdata/defaultmale.txt
//   root motion      meshes/animationdata/boundanims/anims_defaultmale.txt
//
// The DLC creatures' project and root motion are only in
// meshes/animationdatasinglefile.txt, read when a project has no file of its own.
#pragma once

#include "animation_generated.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skydot {

/// Lowercase, forward slashes, `.` and `..` resolved.
std::string normalize_path(const std::string& path);

/// `read(vpath)` returns an asset's bytes, empty if the pack lacks it.
using AssetReader = std::function<std::vector<std::uint8_t>(const std::string&)>;

/// The root of an animation asset's bytes, or null if they are not a verified animation asset of the
/// format this engine reads. The bytes must outlive the pointer.
[[nodiscard]] const bethconv::pack::afb::Animation* animation_asset(const std::vector<std::uint8_t>& bytes);

struct ProjectClip {
    std::string name;
    std::uint32_t animation{}; ///< The id the project's motions are keyed by.
    float speed{1.0F};         ///< The project's playback speed.
    /// The project's text keys of the clip (HitFrame, SoundPlay.*): time in seconds, text.
    std::vector<std::pair<float, std::string>> notes;
};

/// Where the actor is after `time` seconds of the clip at speed 1: game units, Z up, +Y forward; and
/// its turn as a quaternion.
struct ProjectMotion {
    float duration{};
    std::vector<std::array<float, 4>> translations; ///< time, x, y, z
    std::vector<std::array<float, 5>> rotations;    ///< time, x, y, z, w
};

struct AnimationProject {
    std::string folder;  ///< `meshes/actors/character/`
    std::string name;    ///< `defaultmale`
    /// The files it lists (behaviours, characters...), as `normalize_path` spells them, relative to `folder`.
    std::vector<std::string> files;
    std::vector<ProjectClip> clips;
    std::unordered_map<std::uint32_t, ProjectMotion> motions;
    std::string missing; ///< Why there is nothing, or empty.

    [[nodiscard]] const ProjectClip* clip(const std::string& name) const; ///< By name, any case.
};

/// The project of a race's behaviour (`actors/character/defaultmale.hkx`, with or without `meshes/`).
AnimationProject read_animation_project(const std::string& behaviour, const AssetReader& read);

} // namespace skydot
