// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace bethconv::io {

/// Deliberately coarse. `detail` carries specifics; the kind is what callers
/// switch on and what report.json counts.
enum class ErrorKind : std::uint8_t {
    truncated,      ///< Ran off the end of the buffer.
    out_of_range,   ///< Seek/offset outside the buffer.
    unterminated,   ///< String with no NUL before the end of the range.
    bad_magic,      ///< Signature/tag not what the format requires.
    bad_value,      ///< Field present but semantically impossible.
    unsupported,    ///< Well-formed, but a version/feature we do not handle.
    too_large,      ///< Declared size exceeds a sanity limit (decompression bombs).
    corrupt,        ///< Internally inconsistent (offsets crossing, cyclic refs).
};

[[nodiscard]] std::string_view to_string(ErrorKind kind) noexcept;

/// Enough to log "file X, offset Y, expected Z". Only built on failure, so the
/// strings cost nothing on the normal path.
struct ParseError {
    std::string origin;   ///< Virtual path or filename the bytes came from.
    std::size_t offset{}; ///< Absolute offset within `origin`, not within a sub-range.
    ErrorKind kind{ErrorKind::corrupt};
    std::string detail;

    /// "meshes/foo.nif+0x1a4: truncated: reading NiHeader.numBlocks (need 4, have 2)"
    [[nodiscard]] std::string to_string() const;
};

/// Return type of every parser. No exceptions, sentinels or out-params.
template <typename T>
using ParseResult = std::expected<T, ParseError>;

} // namespace bethconv::io
