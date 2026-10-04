// SPDX-License-Identifier: GPL-3.0-or-later
//
// The spelling of a virtual path, as vpath.idx and the pack keep it:
// `Textures\Foo\Bar.dds`, `textures/foo/bar.dds` and `TEXTURES\\FOO\\BAR.DDS`
// all name one asset, written `textures/foo/bar.dds`. The converter writes
// the index with it and the engine looks paths up with it, so a difference
// between the two is a missing asset.
//
// Lowercasing is ASCII-only and locale-independent on purpose: Bethesda's
// hashing is ASCII, and `std::tolower` or Godot's `String::to_lower` change
// letters the converter leaves alone (Turkish dotless i, "Ä").
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace skydot::formats {

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

/// ASCII lowercase, forward slashes, no leading, repeated or trailing
/// separators, `.` segments dropped, `..` kept (vpaths are map keys, not
/// resolved against a filesystem). Bytes above 0x7F are unchanged, so UTF-8
/// passes through.
[[nodiscard]] inline std::string normalize_vpath(std::string_view path) {
    std::string out;
    out.reserve(path.size());

    bool last_was_sep = true; // leading separators are dropped
    std::size_t i = 0;
    while (i < path.size()) {
        char c = path[i];
        if (c == '\\') {
            c = '/';
        }
        if (c == '/') {
            if (!last_was_sep) {
                out.push_back('/');
                last_was_sep = true;
            }
            ++i;
            continue;
        }
        // Drop "./" components.
        if (last_was_sep && c == '.' &&
            (i + 1 == path.size() || path[i + 1] == '/' || path[i + 1] == '\\')) {
            i += 2;
            continue;
        }
        out.push_back(ascii_lower(c));
        last_was_sep = false;
        ++i;
    }

    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

/// `path` under `folder` (which ends in `/`), normalized, unless it already is.
/// Empty stays empty.
[[nodiscard]] inline std::string under_folder(std::string_view path, std::string_view folder) {
    std::string out = normalize_vpath(path);
    if (out.empty() || out.starts_with(folder)) {
        return out;
    }
    return std::string(folder) + out;
}

/// A MODL value is relative to `Data\meshes\`, though some plugins include the
/// prefix: the normalized virtual path.
[[nodiscard]] inline std::string model_vpath(std::string_view modl) {
    return under_folder(modl, "meshes/");
}

/// A TXST path is relative to `Data\textures\`, like MODL to meshes.
[[nodiscard]] inline std::string texture_vpath(std::string_view path) {
    return under_folder(path, "textures/");
}

} // namespace skydot::formats
