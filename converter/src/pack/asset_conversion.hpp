// SPDX-License-Identifier: GPL-3.0-or-later
//
// The conversion of one input of one kind: source bytes in, asset bytes out.
// convert() keeps the work list, the dedupe and the storing; each function here
// depends only on its arguments, so nothing in it needs the pack writer. Private
// to pack/.
//
// A conversion that fails still carries the warnings and counters it produced
// on the way, in the order it produced them: convert() records them as they
// were found, whatever happened next.
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/pack/convert.hpp"
#include "bethconv/pack/pack_writer.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace bethconv::pack {

/// What the texture conversion counted; ConvertResult's `textures_*`.
struct TextureCounters {
    std::uint64_t shrunk{};      ///< Lost top levels to max_texture_size.
    std::uint64_t kept_large{};  ///< Over the limit with no smaller level.
    std::uint64_t bytes_saved{};
    std::uint64_t encoded{};     ///< Uncompressed ones block-compressed.
    std::uint64_t not_encoded{}; ///< Uncompressed but left so (cubemaps, ...).
};

struct AssetConversion {
    /// The asset's bytes, unless `passthrough`.
    std::vector<std::byte> bytes;

    /// The asset is the source bytes as they came (a texture that needed no
    /// edit), so they are not copied.
    bool passthrough = false;

    /// Set if the input did not become an asset.
    std::optional<PackFailure> failure;

    std::vector<PackWarning> warnings;
    TextureCounters textures;

    /// What to store for the input whose bytes are `source`.
    [[nodiscard]] std::span<const std::byte> asset(
        std::span<const std::byte> source) const noexcept {
        return passthrough ? source : std::span<const std::byte>(bytes);
    }
};

[[nodiscard]] PackFailure failure_from(std::string_view vpath, std::string_view stage,
                                       const io::ParseError& error);

[[nodiscard]] AssetConversion convert_mesh(std::span<const std::byte> source,
                                           std::string_view vpath, const ConvertOptions& options);

[[nodiscard]] AssetConversion convert_texture(std::span<const std::byte> source,
                                              std::string_view vpath,
                                              const ConvertOptions& options);

[[nodiscard]] AssetConversion convert_script(std::span<const std::byte> source,
                                             std::string_view vpath);

/// `extension` is the vpath's, with the dot.
[[nodiscard]] AssetConversion convert_lod(std::span<const std::byte> source,
                                          std::string_view vpath, std::string_view extension);

/// Havok files and the `animationdata` text files.
[[nodiscard]] AssetConversion convert_animation(std::span<const std::byte> source,
                                                std::string_view vpath);

} // namespace bethconv::pack
