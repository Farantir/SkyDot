// SPDX-License-Identifier: GPL-3.0-or-later
//
// Steam's text KeyValues format, as in `libraryfolders.vdf` and
// `appmanifest_<appid>.acf`:
//
//   "libraryfolders"
//   {
//       "0"
//       {
//           "path"      "/home/user/.local/share/Steam"
//           "apps"      { "489830"  "16092533099" }
//       }
//   }
//
// Quoted or bare tokens, `{` `}` for nesting, `//` comments, and the escapes
// `\\` `\"` `\n` `\t`. Steam writes Windows paths with doubled backslashes.
// Observed in files written by the Steam client; Valve documents the format
// only informally (developer.valvesoftware.com/wiki/KeyValues).
#pragma once

#include "bethconv/io/parse_error.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::install {

/// One key with either a string value or children.
struct VdfNode {
    std::string key;
    std::string value;
    std::vector<VdfNode> children;
    bool is_object = false;

    /// First child named `key` (case-insensitive, as Steam's own reader), or
    /// nullptr.
    [[nodiscard]] const VdfNode* find(std::string_view key) const noexcept;

    /// Value of child `key`, or empty.
    [[nodiscard]] std::string_view get(std::string_view key) const noexcept;
};

/// Parse a whole file's text. The result is an object node with an empty key
/// holding the top-level entries.
[[nodiscard]] io::ParseResult<VdfNode> parse_vdf(std::string_view text);

[[nodiscard]] io::ParseResult<VdfNode> read_vdf(const std::filesystem::path& path);

} // namespace bethconv::install
