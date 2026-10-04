// SPDX-License-Identifier: GPL-3.0-or-later
//
// The kinds of asset a pack holds and how each is stored: its word in
// vpath.idx, its extension on disk and where the loose file lives. The
// converter writes them and the engine reads them back.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace skydot::formats {

/// One extension and one settings fingerprint each.
enum class AssetKind : std::uint8_t {
    mesh,      ///< NIF -> GLB.
    texture,   ///< DDS -> DDS, mip chain completed.
    script,    ///< PEX -> decoded FlatBuffer (formats/schema/script.fbs).
    lod,       ///< .lod/.lst/.btt -> decoded FlatBuffer (formats/schema/lod.fbs).
    animation, ///< .hkx, animationdata .txt -> decoded FlatBuffer (formats/schema/animation.fbs).
};

inline constexpr std::array<AssetKind, 5> k_asset_kinds{AssetKind::mesh, AssetKind::texture,
                                                       AssetKind::script, AssetKind::lod,
                                                       AssetKind::animation};

/// The word vpath.idx writes for a kind.
[[nodiscard]] constexpr std::string_view to_string(AssetKind kind) noexcept {
    switch (kind) {
    case AssetKind::mesh: return "mesh";
    case AssetKind::texture: return "texture";
    case AssetKind::script: return "script";
    case AssetKind::lod: return "lod";
    case AssetKind::animation: return "animation";
    }
    return "unknown";
}

/// Including the dot: ".glb", ".dds", ".pexfb", ".lodfb", ".animfb".
[[nodiscard]] constexpr std::string_view extension_of(AssetKind kind) noexcept {
    switch (kind) {
    // DDS, not KTX2: textures are passed through.
    case AssetKind::mesh: return ".glb";
    case AssetKind::texture: return ".dds";
    case AssetKind::script: return ".pexfb";
    case AssetKind::lod: return ".lodfb";
    case AssetKind::animation: return ".animfb";
    }
    return "";
}

/// Inverse of `to_string`. Nullopt for an unknown word, i.e. a pack from a
/// newer converter.
[[nodiscard]] constexpr std::optional<AssetKind> kind_from_string(std::string_view name) noexcept {
    for (const auto kind : k_asset_kinds) {
        if (name == to_string(kind)) {
            return kind;
        }
    }
    return std::nullopt;
}

/// `assets/<bb>/<hex><ext>` with forward slashes on every platform: the loose
/// file of the asset with this content hash, relative to the pack root.
[[nodiscard]] inline std::string asset_relative_path(std::string_view hex, AssetKind kind) {
    std::string out = "assets/";
    out += hex.substr(0, std::min<std::size_t>(2, hex.size()));
    out += '/';
    out += hex;
    out += extension_of(kind);
    return out;
}

} // namespace skydot::formats
