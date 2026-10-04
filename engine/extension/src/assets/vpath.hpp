// SPDX-License-Identifier: GPL-3.0-or-later
//
// The spelling of a virtual path in vpath.idx, and ASCII lowercasing.
//
// Both are locale-independent on purpose: `std::tolower` and Godot's
// `String::to_lower` change letters the converter leaves alone (Turkish
// dotless i, "Ä"), and then a path no longer finds its asset.
#pragma once

#include <string>
#include <string_view>

namespace skydot {

/// `A-Z` to `a-z`; every other byte, UTF-8 continuation bytes included, is
/// kept.
[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] inline std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = ascii_lower(c);
    }
    return out;
}

/// The key vpath.idx uses: ASCII lowercase, backslashes to slashes, no leading,
/// repeated or trailing separators, "." segments dropped, ".." kept. Bytes
/// above 0x7F are unchanged, so UTF-8 passes through.
///
/// Mirrors `bethconv::archive::normalize_vpath` (converter/src/archive/vpath.cpp),
/// which is the definition; change both together.
[[nodiscard]] std::string normalize_vpath(std::string_view path);

} // namespace skydot
