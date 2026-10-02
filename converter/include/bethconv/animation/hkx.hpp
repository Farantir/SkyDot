// SPDX-License-Identifier: GPL-3.0-or-later
//
// Skyrim's Havok files (.hkx): binary packfiles, version 8, hk_2010.2.0-r1.
// LE writes 4-byte pointers, SE 8-byte; nothing else differs. The files carry
// no type information, so the members of the classes read here sit at fixed
// offsets per pointer size. Format and measurements:
// converter/docs/spikes/hkx.md.
//
// Read: skeletons (hkaSkeleton), animation bindings, and spline-compressed
// or interleaved animations with their annotations. Behaviour graphs,
// physics and everything else are counted by class and skipped.
//
// Animations are kept as splines, not sampled: every vanilla SE clip sampled
// per frame would take 1 GiB, the splines 54 MiB. A channel's control points
// are requantized to 16 bits between a per-axis minimum and maximum, which is
// lossless for positions and scales (8- and 16-bit sources) and finer than
// the 12-bit rotation sources.
#pragma once

#include "bethconv/io/parse_error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::animation {

/// Translation, rotation (quaternion xyzw), scale: Havok's hkQsTransform.
struct QsTransform {
    std::array<float, 3> translation{0, 0, 0};
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::array<float, 3> scale{1, 1, 1};
};

struct Skeleton {
    std::string name;
    std::vector<std::int16_t> parents; ///< -1 for a root.
    std::vector<std::string> bones;
    std::vector<QsTransform> reference_pose;
    std::vector<std::string> float_slots;
};

/// One property of one track within one block.
///
/// - `lo` empty: the property's identity (no translation, no rotation, unit
///   scale).
/// - `knots` empty: constant `lo`.
/// - otherwise a B-spline of `degree` with `knots` in frames within the block
///   and `points.size() / lo.size()` control points; axis `a` of point `i` is
///   `lo[a] + (hi[a] - lo[a]) * points[i * width + a] / 65535`.
///
/// Width is 3 for translation and scale, 4 for rotation, 1 for float tracks.
struct Channel {
    std::uint8_t degree{};
    std::vector<std::uint8_t> knots;
    std::vector<float> lo;
    std::vector<float> hi;
    std::vector<std::uint16_t> points;

    [[nodiscard]] std::size_t width() const noexcept { return lo.size(); }
    [[nodiscard]] bool is_spline() const noexcept { return !knots.empty(); }
};

struct Track {
    Channel translation;
    Channel rotation;
    Channel scale;
};

struct Block {
    std::vector<Track> tracks;
    std::vector<Channel> floats;
};

struct Annotation {
    float time{};
    std::string text;
};

struct AnnotationTrack {
    std::string name;
    std::vector<Annotation> annotations;
};

/// The class an animation was stored as.
enum class ClipEncoding : std::uint8_t { spline, interleaved };

/// An animation with its binding. Frame `f` lives in block
/// `min(f / (frames_per_block - 1), blocks - 1)` at local frame
/// `f - block * (frames_per_block - 1)`: neighbouring blocks share a frame.
/// Interleaved animations become one block of linear splines with a knot per
/// frame.
struct Clip {
    ClipEncoding encoding{};
    float duration{};
    std::uint32_t frame_count{};
    float frame_duration{};
    std::uint32_t frames_per_block{};
    std::uint32_t transform_tracks{};
    std::uint32_t float_tracks{};
    std::vector<Block> blocks;
    std::vector<AnnotationTrack> annotations;
    /// From the binding; empty when the file has none. `track_to_bone` empty
    /// means track i drives bone i.
    std::string skeleton_name;
    std::vector<std::int16_t> track_to_bone;
    std::vector<std::int16_t> float_to_slot;
    std::uint8_t blend_hint{}; ///< 0 normal, 1 additive.
    /// Class of the extracted motion object, when there is one. Not decoded:
    /// Skyrim's root motion lives in the animationdata text files.
    std::string extracted_motion;
};

struct HkxFile {
    std::uint8_t pointer_size{};
    std::string version;
    std::vector<Skeleton> skeletons;
    std::vector<Clip> clips;
    /// Every object's class, counted, including those not read.
    std::map<std::string, std::uint32_t, std::less<>> classes;
};

/// Limits on what one file may claim; vanilla stays far below them.
inline constexpr std::uint32_t k_max_tracks = 4096;
inline constexpr std::uint32_t k_max_frames = 1u << 20;

/// Read a packfile. Malformed input, an unknown layout or version, and an
/// unsupported rotation quantization are errors; unknown classes are not.
/// A Havok binary tagfile (6 vanilla SE files, Creation Club fishing) is
/// `unsupported`.
[[nodiscard]] io::ParseResult<HkxFile> read_hkx(std::span<const std::byte> bytes,
                                                std::string_view origin);

/// The value of `channel` at local frame `u` of its block, `width()` floats.
/// Rotations come back as blended, not normalized, quaternions.
void evaluate(const Channel& channel, float u, std::span<float> out) noexcept;

/// Sample a whole track at clip frame `frame` (rotation normalized).
[[nodiscard]] QsTransform sample(const Clip& clip, std::uint32_t track, std::uint32_t frame) noexcept;

} // namespace bethconv::animation
