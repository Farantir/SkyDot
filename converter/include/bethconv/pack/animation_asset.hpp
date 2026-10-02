// SPDX-License-Identifier: GPL-3.0-or-later
//
// Animation assets: a Havok file's skeletons, clips and characters, or an
// animationdata file's clips and motions, as a FlatBuffer (schema
// formats/schema/animation.fbs), so the engine never parses HKX or the text.
#pragma once

#include "bethconv/animation/animation_data.hpp"
#include "bethconv/animation/hkx.hpp"
#include "bethconv/io/parse_error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace bethconv::pack {

/// Bumped whenever the meaning of anything in animation.fbs changes.
inline constexpr std::uint32_t k_animation_format_version = 1;

[[nodiscard]] std::vector<std::byte> write_animation_asset(const animation::HkxFile& file);

/// Verify an animation asset and read it back. `classes` is not stored.
[[nodiscard]] io::ParseResult<animation::HkxFile> read_animation_asset(std::span<const std::byte> bytes,
                                                                       std::string_view origin);

[[nodiscard]] std::vector<std::byte> write_animation_asset(const animation::ProjectData& data);

/// Verify an animation asset and read back its project files, clips and
/// motions.
[[nodiscard]] io::ParseResult<animation::ProjectData> read_project_asset(std::span<const std::byte> bytes,
                                                                         std::string_view origin);

} // namespace bethconv::pack
