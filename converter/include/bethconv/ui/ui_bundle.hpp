// SPDX-License-Identifier: GPL-3.0-or-later
//
// A menu movie's assets as files (docs/adr/0010-menus-and-skyui-compatibility.md):
//
//   manifest.json        what the movie holds: fonts, bitmaps, characters with
//                        their bounds, frames, labels, scaling grids and first
//                        display list, text fields, exports and imports
//   fonts/<name>.ttf     each DefineFont2/3 with glyphs, as a TrueType font
//   png/<id>.png         each lossless bitmap (straight alpha); jpg/<id>.jpg
//   svg/<id>.svg         each shape, button (up state) and sprite (first frame)
//   svg/<id>_f<n>.svg    the other labelled frames of a sprite, and every frame
//                        of an exported sprite of at most k_all_frames_limit
//   svg/root.svg         the main timeline's first frame on the stage
//
// In the pack the files are one ZIP (stored or deflated, no timestamps), the
// asset of the movie's vpath. Text fields and imported characters are not
// drawn into the SVGs: the manifest places them.
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/ui/swf.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::ui {

/// Exported sprites with up to this many frames get every frame drawn.
inline constexpr std::uint32_t k_all_frames_limit = 32;

/// Version of the bundle layout and manifest keys.
inline constexpr int k_bundle_version = 1;

struct BundleFile {
    std::string path;
    std::vector<std::byte> bytes;
};

/// RGBA8 rows top-down to a PNG (colour type 6, filter 0, zlib level 6).
[[nodiscard]] std::vector<std::byte> write_png(std::uint32_t width, std::uint32_t height,
                                               std::span<const std::uint8_t> rgba);

/// A DefineFont2/3 as a TrueType font: quadratic outlines from the glyph
/// shapes (y flipped, 2048 units per em), cmap format 4 from the code table,
/// hmtx from the advances, kern format 0 from the kerning table. Nullopt for a
/// font without glyphs (a device font reference).
[[nodiscard]] std::optional<std::vector<std::byte>> write_ttf(const Font& font);

/// One character drawn at frame `frame` as a standalone SVG (pixels), its
/// viewBox the character's bounds. Nullopt if the id is not drawable.
[[nodiscard]] std::optional<std::string> character_svg(const SwfMovie& movie, std::uint16_t id,
                                                       std::uint32_t frame = 0);

/// The main timeline at `frame` on the stage rectangle.
[[nodiscard]] std::string root_svg(const SwfMovie& movie, std::uint32_t frame = 0);

/// Bounds of a character at a frame in its own space, twips. Empty if none.
[[nodiscard]] Rect character_bounds(const SwfMovie& movie, std::uint16_t id,
                                    std::uint32_t frame = 0);

/// Every file of the bundle, in a fixed order, manifest first.
[[nodiscard]] std::vector<BundleFile> bundle_files(const SwfMovie& movie, std::string_view vpath);

/// A ZIP of `files` (deflated where smaller, DOS time 1980-01-01).
[[nodiscard]] std::vector<std::byte> write_zip(std::span<const BundleFile> files);

/// Read a movie and pack its bundle as a ZIP: the converter's UI asset.
[[nodiscard]] io::ParseResult<std::vector<std::byte>> convert_swf(std::span<const std::byte> swf,
                                                                  std::string_view vpath);

/// A UI text file (interface/fontconfig.txt, translate_*.txt): UTF-16LE with a
/// byte order mark becomes UTF-8, CRLF stays; anything else is kept as is.
/// The asset is a ZIP with the one file under its base name.
[[nodiscard]] std::vector<std::byte> convert_ui_text(std::span<const std::byte> text,
                                                     std::string_view vpath);

} // namespace bethconv::ui
