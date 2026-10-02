// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyrim's animationdata text files: what a behaviour project's clips are
// called and how far each animation moves its actor (root motion, which the
// HKX files do not carry). Both editions ship the same files (323 projects,
// 39 with clips, 5,456 motions) and `meshes/animationdatasinglefile.txt`
// (LE in Update.bsa) with 429 projects in one file, the DLC creatures' only
// there:
//
//     <n>                      project count
//     ChickenProject.txt       n project names
//     <lines>                  per project: a project file of that many lines
//     ...
//     <lines>                  and, if it has clips, a boundanims file
//     ...
//
// `meshes/animationdata/<project>.txt`, CRLF lines:
//
//     1                       (always 1)
//     <n>                     the project's files, relative to its folder:
//     Behaviors\0_Master.hkx  behaviours, the character, its rig, and for
//     ...                     projects without clips the animations
//     <0|1>                   1: clip generators follow
//     MT_WalkForward          clip generator name (may contain spaces); the
//                             behaviour graph's hkbClipGenerator of this
//                             name says which file it plays
//     965                     the clip's id, which motions are keyed by
//     1                       playback speed
//     0                       crop start
//     0                       crop end
//     2                       annotation count
//     FootLeft:0.2            text:time
//     FootRight:0.766667
//     (blank)
//
// `meshes/animationdata/boundanims/anims_<project>.txt`, per clip id that
// moves: index, duration, translation key count, keys `time x y z`,
// rotation key count, keys `time x y z w`, blank. Game units, Z up, +Y
// forward; `MT_WalkForward` (965) is 93.4 units forward in 1.133 s.
#pragma once

#include "bethconv/io/parse_error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::animation {

struct ProjectClip {
    std::string name;
    std::uint32_t animation{}; ///< The clip's id, which motions are keyed by.
    float speed{1.0F};         ///< Playback speed.
    float crop_start{};
    float crop_end{};
    std::vector<std::pair<float, std::string>> annotations; ///< (time, text)
};

struct MotionKey {
    float time{};
    std::array<float, 3> translation{};
};

struct RotationKey {
    float time{};
    std::array<float, 4> rotation{0, 0, 0, 1}; ///< x, y, z, w
};

/// Where an animation leaves the actor after `time`, from its start.
struct Motion {
    std::uint32_t animation{};
    float duration{};
    std::vector<MotionKey> translations;
    std::vector<RotationKey> rotations;
};

/// One animationdata file: a project file has files and clips, a boundanims
/// file motions, the single file `projects`, each with all three.
struct ProjectData {
    std::string name; ///< In `projects`: "ChickenProject" (".txt" dropped).
    std::vector<std::string> files;
    std::vector<ProjectClip> clips;
    std::vector<Motion> motions;
    std::vector<ProjectData> projects;
    /// A project file's clip flag; in the single file, motions follow it.
    bool has_clips{};
};

/// Limits on what one file may claim; vanilla stays far below them.
inline constexpr std::uint32_t k_max_project_entries = 1u << 16;

/// Whether a virtual path (lowercase, forward slashes) is an animationdata
/// file this module reads.
[[nodiscard]] bool is_animation_data(std::string_view vpath) noexcept;

/// Read a project file (`meshes/animationdata/<project>.txt`).
[[nodiscard]] io::ParseResult<ProjectData> read_project(std::span<const std::byte> bytes,
                                                        std::string_view origin);

/// Read a boundanims file (`meshes/animationdata/boundanims/anims_*.txt`).
[[nodiscard]] io::ParseResult<ProjectData> read_bound_anims(std::span<const std::byte> bytes,
                                                            std::string_view origin);

/// Read `meshes/animationdatasinglefile.txt`.
[[nodiscard]] io::ParseResult<ProjectData> read_single_file(std::span<const std::byte> bytes,
                                                            std::string_view origin);

/// Any of the three, chosen by the path.
[[nodiscard]] io::ParseResult<ProjectData> read_animation_data(std::span<const std::byte> bytes,
                                                               std::string_view vpath);

} // namespace bethconv::animation
