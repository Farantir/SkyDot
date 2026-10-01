// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/io/output_target.hpp"

#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__linux__)
#include <sys/stat.h>
#include <sys/sysmacros.h>
#endif

namespace bethconv::io {
namespace {

#if defined(__linux__)

/// mountinfo escapes space, tab, newline and backslash as \ooo.
std::string unescape_octal(std::string_view text) {
    const auto octal = [](char c) { return c >= '0' && c <= '7'; };
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && text.size() - i >= 4 && octal(text[i + 1]) && octal(text[i + 2]) &&
            octal(text[i + 3])) {
            out.push_back(static_cast<char>((text[i + 1] - '0') * 64 + (text[i + 2] - '0') * 8 +
                                            (text[i + 3] - '0')));
            i += 3;
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string read_line(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

/// The closest existing ancestor, absolute and with links resolved.
std::filesystem::path existing_ancestor(const std::filesystem::path& path) {
    std::error_code ec;
    auto current = std::filesystem::absolute(path, ec);
    while (!current.empty()) {
        if (std::filesystem::exists(current, ec)) {
            auto resolved = std::filesystem::canonical(current, ec);
            return ec ? current : resolved;
        }
        if (current == current.parent_path()) {
            break;
        }
        current = current.parent_path();
    }
    return {};
}

bool under(const std::string& path, const std::string& mount) {
    if (mount == "/") {
        return true;
    }
    return path == mount || (path.starts_with(mount) && path[mount.size()] == '/');
}

/// The disk behind a block device: the partition's parent, or the device.
void find_disk(OutputTarget& target) {
    if (!target.source.starts_with("/dev/")) {
        return;
    }
    struct stat st {};
    std::error_code ec;
    const auto device = std::filesystem::canonical(target.source, ec);
    if (ec || ::stat(device.c_str(), &st) != 0 || !S_ISBLK(st.st_mode)) {
        return;
    }
    const auto sys = std::filesystem::path("/sys/dev/block") /
                     (std::to_string(major(st.st_rdev)) + ":" + std::to_string(minor(st.st_rdev)));
    auto node = std::filesystem::canonical(sys, ec);
    if (ec) {
        return;
    }
    if (std::filesystem::exists(node / "partition", ec)) {
        node = node.parent_path();
    }
    target.disk = node.filename().string();
    const auto rotational = read_line(node / "queue/rotational");
    if (rotational == "0" || rotational == "1") {
        target.rotational = rotational == "1";
    }
    target.zoned = read_line(node / "queue/zoned");
}

#endif

} // namespace

std::string OutputTarget::describe() const {
    std::string out = fs_type + " on " + source;
    std::vector<std::string> notes;
    if (!disk.empty()) {
        notes.push_back(disk);
    }
    if (rotational) {
        notes.push_back(*rotational ? "rotational" : "solid state");
    }
    if (!zoned.empty() && zoned != "none") {
        notes.push_back("zoned " + zoned);
    }
    if (!notes.empty()) {
        out += " (";
        for (std::size_t i = 0; i < notes.size(); ++i) {
            out += (i == 0 ? "" : ", ") + notes[i];
        }
        out += ")";
    }
    return out;
}

std::optional<OutputTarget> probe_output_target(const std::filesystem::path& path) {
#if defined(__linux__)
    const auto existing = existing_ancestor(path);
    if (existing.empty()) {
        return std::nullopt;
    }
    const std::string where = existing.string();

    std::ifstream mountinfo("/proc/self/mountinfo");
    if (!mountinfo) {
        return std::nullopt;
    }
    std::optional<OutputTarget> best;
    std::string line;
    while (std::getline(mountinfo, line)) {
        // id parent major:minor root mount-point options [optional...] - type source super
        std::istringstream fields(line);
        std::string id, parent, devno, root, mount_point;
        fields >> id >> parent >> devno >> root >> mount_point;
        std::string field;
        while (fields >> field && field != "-") {
        }
        std::string type, source;
        fields >> type >> source;
        mount_point = unescape_octal(mount_point);
        if (type.empty() || !under(where, mount_point)) {
            continue;
        }
        // Later lines for the same point are mounts on top; longer is closer.
        if (!best || mount_point.size() >= best->mount_point.size()) {
            OutputTarget target;
            target.mount_point = mount_point;
            target.fs_type = type;
            target.source = unescape_octal(source);
            target.fuse = type == "fuse" || type == "fuseblk" || type.starts_with("fuse.");
            best = std::move(target);
        }
    }
    if (best) {
        find_disk(*best);
    }
    return best;
#else
    (void)path;
    return std::nullopt;
#endif
}

} // namespace bethconv::io
