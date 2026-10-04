// SPDX-License-Identifier: GPL-3.0-or-later
//
// Virtual path normalization. `Textures\Foo\Bar.dds`, `textures/foo/bar.dds`
// and `TEXTURES\\FOO\\BAR.DDS` all name the same asset; everything above the
// archive layer uses the normalized form.
#pragma once

#include "skydot_formats/vpath.hpp"

#include <string>
#include <string_view>

namespace bethconv::archive {

/// Lowercase ASCII, forward slashes, no leading slash, no duplicate separators,
/// no `.` components. The engine looks paths up with the same function
/// (formats/include/skydot_formats/vpath.hpp).
///
/// ASCII-only because Bethesda's hashing is ASCII; locale-aware lowercasing
/// would make lookups locale-dependent (Turkish dotless i).
using skydot::formats::normalize_vpath;

/// True if `path` is already in canonical form.
[[nodiscard]] bool is_normalized(std::string_view path);

/// The extension without the dot, lowercased, or empty. `"foo/bar.DDS"` -> `"dds"`.
[[nodiscard]] std::string_view vpath_extension(std::string_view normalized);

/// True if `vpath` can be joined onto an output directory without escaping it:
/// relative, forward slashes only, no drive or scheme colon, and no empty, `.`
/// or `..` segments. Virtual paths come from mod archives, so check before
/// writing.
[[nodiscard]] bool is_safe_relative(std::string_view vpath) noexcept;

/// The leading directory component, or empty. `"textures/foo/b.dds"` -> `"textures"`.
[[nodiscard]] std::string_view vpath_top_folder(std::string_view normalized);

} // namespace bethconv::archive
