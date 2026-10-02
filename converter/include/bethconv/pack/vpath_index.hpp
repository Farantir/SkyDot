// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vpath.idx`: maps virtual paths to content hashes. Assets are stored under
// the hash of their source bytes (pack/asset_store.hpp), so this is the only
// way to find an asset by its game path.
//
// pack_writer.cpp writes it and pack_view.cpp reads it; the format and the
// shared layout vocabulary (asset kinds, extensions, paths) live here so both
// sides use one implementation.
//
//     # bethconv vpath index v1
//     # virtual path\tcontent hash\tkind\twinning source
//     meshes/clutter/apple01.nif\t3f9c...\tmesh\tSkyrim - Meshes0.bsa
//
// Tab-separated, sorted, one line per virtual path. Text because it is used to
// debug mod overrides (which archive won, did it change), so people read it and
// tools diff it. About 20 MB for vanilla SE, 3% of the pack.
//
// Several paths with one hash is normal (dedupe); each gets a line.
#pragma once

#include "bethconv/io/parse_error.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::pack {

/// Bumped whenever the meaning of anything in the pack layout changes. See
/// formats/pack-format.md.
inline constexpr std::uint32_t k_pack_format_version = 6;

/// Asset kind. One extension and one settings fingerprint each.
enum class AssetKind : std::uint8_t {
    mesh,    ///< NIF -> GLB.
    texture, ///< DDS -> DDS, mip chain completed.
    script,  ///< PEX -> decoded FlatBuffer (formats/schema/script.fbs).
    lod,     ///< .lod/.lst/.btt -> decoded FlatBuffer (formats/schema/lod.fbs).
    animation, ///< .hkx, animationdata .txt -> decoded FlatBuffer (formats/schema/animation.fbs).
};

[[nodiscard]] std::string_view to_string(AssetKind kind) noexcept;

/// Including the dot: ".glb", ".dds", ".pexfb", ".lodfb", ".animfb".
[[nodiscard]] std::string_view extension_of(AssetKind kind) noexcept;

/// Inverse of `to_string`. Nullopt for an unknown word, i.e. a pack from a
/// newer converter.
[[nodiscard]] std::optional<AssetKind> kind_from_string(std::string_view name) noexcept;

/// `assets/<bb>/<hex><ext>` with forward slashes on every platform, since it
/// is compared with paths read from disk.
[[nodiscard]] std::string asset_relative_path(std::string_view hex, AssetKind kind);

/// One line of the index.
struct VpathEntry {
    std::string vpath;  ///< Lowercase, forward slashes.
    std::string hex;    ///< 64 lowercase hex characters.
    AssetKind kind{};
    std::string source; ///< Which mounted archive or directory won the path.

    /// Location of the asset, relative to the pack root.
    [[nodiscard]] std::string asset_path() const { return asset_relative_path(hex, kind); }
};

/// The two header lines every `vpath.idx` starts with, newline included.
[[nodiscard]] std::string index_header();

    /// One data line, newline included.
[[nodiscard]] std::string format_index_line(std::string_view vpath, std::string_view hex,
                                            AssetKind kind, std::string_view source);

/// A parsed `vpath.idx`, sorted and searchable. A malformed line is an error,
/// not skipped: this file is the only way from a path to its bytes.
class VpathIndex {
public:
    [[nodiscard]] static io::ParseResult<VpathIndex> read(const std::filesystem::path& path);

    /// The same, over text already in hand. `origin` names it in errors.
    [[nodiscard]] static io::ParseResult<VpathIndex> parse(std::string_view text,
                                                           std::string_view origin);

    [[nodiscard]] const std::vector<VpathEntry>& entries() const noexcept { return entries_; }

    /// Version from the file's header line; the caller decides what to do
    /// with a newer one.
    [[nodiscard]] std::uint32_t format_version() const noexcept { return format_version_; }

    /// Entry for `vpath`, or null. Binary search over the sorted entries.
    [[nodiscard]] const VpathEntry* find(std::string_view vpath) const noexcept;

private:
    std::vector<VpathEntry> entries_;
    std::uint32_t format_version_{};
};

} // namespace bethconv::pack
