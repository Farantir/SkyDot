// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/archive/vpath.hpp"

#include <algorithm>

namespace bethconv::archive {
namespace {

constexpr char to_lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

std::string normalize_vpath(std::string_view path) {
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
        // Drop "./" components. ".." is kept: vpaths are map keys and are not
        // resolved against the filesystem here.
        if (last_was_sep && c == '.' &&
            (i + 1 == path.size() || path[i + 1] == '/' || path[i + 1] == '\\')) {
            i += 2;
            continue;
        }
        out.push_back(to_lower_ascii(c));
        last_was_sep = false;
        ++i;
    }

    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

bool is_normalized(std::string_view path) { return normalize_vpath(path) == path; }

std::string_view vpath_extension(std::string_view normalized) {
    const auto dot = normalized.rfind('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    const auto slash = normalized.rfind('/');
    if (slash != std::string_view::npos && dot < slash) {
        return {}; // the dot is in a directory name
    }
    return normalized.substr(dot + 1);
}

bool is_safe_relative(std::string_view vpath) noexcept {
    if (vpath.empty() || vpath.front() == '/' || vpath.front() == '\\') {
        return false;
    }
    if (vpath.find('\\') != std::string_view::npos || vpath.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= vpath.size()) {
        const auto end = vpath.find('/', start);
        const auto segment =
            vpath.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (segment == ".." || segment == "." || segment.empty()) {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

std::string_view vpath_top_folder(std::string_view normalized) {
    const auto slash = normalized.find('/');
    return slash == std::string_view::npos ? std::string_view{}
                                           : normalized.substr(0, slash);
}

} // namespace bethconv::archive
