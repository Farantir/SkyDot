// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/archive/vpath.hpp"

#include <cstddef>

namespace bethconv::archive {

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
