// SPDX-License-Identifier: GPL-3.0-or-later
#include "assets/vpath.hpp"

namespace skydot {

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
        // resolved against the filesystem.
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

} // namespace skydot
