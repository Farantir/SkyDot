// SPDX-License-Identifier: GPL-3.0-or-later
//
// Makes strings from game files safe to put into JSON.
//
// Two sinks need it:
//
//   - glTF names and URIs: RFC 8259 §7 forbids bytes below 0x20, and fastgltf
//     0.9.0 only escapes `"` and `\`, so strict readers (Blender 5.1) reject
//     the file.
//   - `nlohmann::json::dump()` throws on invalid UTF-8.
//
// Only those bytes are encoded; everything else, including valid multi-byte
// UTF-8, passes through, and the escaping is reversible.
#pragma once

#include <string>
#include <string_view>

namespace bethconv::io {

/// Percent-encodes bytes below 0x20 and bytes that are not part of well-formed
/// UTF-8; everything else is unchanged.
///
/// Encoding instead of stripping keeps the original recoverable: 67 vanilla SE
/// FaceGen meshes name their normal map `textures\<0x08>NOR`, which becomes
/// `textures/%08nor`.
///
/// The result is JSON-safe, not a URI; URIs need `uri_escape` in the glTF
/// writer on top.
[[nodiscard]] std::string json_text(std::string_view text);

/// Length of the well-formed UTF-8 sequence at `i`, or 0 if there is none.
/// Implements Unicode 15 table 3-7, rejecting overlongs, surrogates and values
/// above U+10FFFF. Game strings are often cp1252, so high bytes are not assumed
/// to be UTF-8.
[[nodiscard]] std::size_t utf8_sequence_length(std::string_view text, std::size_t i);

} // namespace bethconv::io
