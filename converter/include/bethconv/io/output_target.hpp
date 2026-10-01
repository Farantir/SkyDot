// SPDX-License-Identifier: GPL-3.0-or-later
//
// What kind of storage an output path is on, so the CLI can refuse writes that
// overwhelm it. Writing a pack as ~70,000 loose files to an SMR disk behind
// ntfs-3g (a FUSE driver) once hung the whole mount until a hard reset, which
// corrupted unrelated directories on it.
//
// Linux only: the mount comes from /proc/self/mountinfo, the disk from
// /sys/dev/block. Elsewhere, and when anything is unreadable, nothing is known
// and nothing is refused. Drive-managed SMR disks report `zoned` as "none", so
// a spinning disk is the signal, not SMR itself.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace bethconv::io {

struct OutputTarget {
    std::string mount_point;
    std::string fs_type;  ///< As mounted: "ext4", "fuseblk", "tmpfs", ...
    std::string source;   ///< "/dev/sdd2", or what the filesystem names.
    std::string disk;     ///< Block device under it ("sdd"), if found.
    bool fuse = false;    ///< Userspace filesystem (ntfs-3g mounts as fuseblk).
    std::optional<bool> rotational;
    std::string zoned;    ///< "none", "host-aware", "host-managed"; empty if unknown.

    /// Spinning, zoned or FUSE: fine for a few large files, not for many
    /// small ones.
    [[nodiscard]] bool slow_for_many_files() const {
        return fuse || rotational.value_or(false) || (!zoned.empty() && zoned != "none");
    }

    /// One line for the terminal: "fuseblk on /dev/sdd2 (sdd, rotational)".
    [[nodiscard]] std::string describe() const;
};

/// Probe the storage under `path`, which need not exist yet.
[[nodiscard]] std::optional<OutputTarget> probe_output_target(const std::filesystem::path& path);

} // namespace bethconv::io
