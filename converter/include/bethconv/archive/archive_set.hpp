// SPDX-License-Identifier: GPL-3.0-or-later
//
// All BSAs, BA2s and loose files of an install as one virtual filesystem, with
// the game's precedence rules. Callers ask for a path and get bytes.
#pragma once

#include "bethconv/archive/vpath.hpp"
#include "bethconv/io/mapped_file.hpp"
#include "bethconv/io/span_reader.hpp"

#include <functional>
#include <optional>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bethconv::archive {

enum class SourceKind : std::uint8_t {
    loose,  ///< A plain directory on disk.
    tes4,   ///< BSA (Oblivion .. Skyrim SE).
    fo4,    ///< BA2 (Fallout 4, Fallout 76, Starfield).
    tes3,   ///< Morrowind BSA. Detected only to report it as unsupported.
};

[[nodiscard]] std::string_view to_string(SourceKind kind) noexcept;

/// One mounted provider of files.
struct SourceInfo {
    std::string name;             ///< Display name, usually the filename.
    std::filesystem::path path;
    SourceKind kind{};
    int priority{};               ///< Higher wins. See ArchiveSet::mount_*.
    std::size_t file_count{};
    std::uint32_t version{};      ///< Archive format version, 0 for loose.
};

/// Where one virtual path can be found, and who wins.
struct Resolution {
    std::string vpath;
    std::size_t winner{};                 ///< Index into sources().
    std::vector<std::size_t> shadowed;    ///< Losers, highest priority first.

    [[nodiscard]] bool is_conflict() const noexcept { return !shadowed.empty(); }
};

/// Archives and loose directories mounted in precedence order:
/// - higher `priority` wins;
/// - at equal priority, loose beats archived;
/// - otherwise the later mount wins.
///
/// The index is built at mount time: lookups are one hash probe and conflicts
/// come for free.
class ArchiveSet {
public:
    ArchiveSet();
    ~ArchiveSet();
    ArchiveSet(const ArchiveSet&) = delete;
    ArchiveSet& operator=(const ArchiveSet&) = delete;
    ArchiveSet(ArchiveSet&&) noexcept;
    ArchiveSet& operator=(ArchiveSet&&) noexcept;

    /// Mount a .bsa/.ba2. Format is detected from content, not the extension.
    [[nodiscard]] io::ParseResult<std::size_t> mount_archive(
        const std::filesystem::path& path, int priority);

    /// Mount a directory of loose files, indexed recursively.
    [[nodiscard]] io::ParseResult<std::size_t> mount_loose(
        const std::filesystem::path& dir, int priority);

    /// Read a file by virtual path (need not be normalized), decompressing if
    /// needed.
    [[nodiscard]] io::ParseResult<std::vector<std::byte>> read(
        std::string_view path) const;

    /// Look up without reading; nullopt if no source has the path.
    [[nodiscard]] std::optional<Resolution> resolve(std::string_view path) const;

    [[nodiscard]] const std::vector<SourceInfo>& sources() const noexcept {
        return sources_;
    }
    [[nodiscard]] std::size_t unique_paths() const noexcept;

    /// Every path provided by more than one source, in arbitrary order.
    [[nodiscard]] std::vector<Resolution> conflicts() const;

    /// Iterate every distinct virtual path. `fn(const Resolution&)`.
    void for_each(const std::function<void(const Resolution&)>& fn) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::vector<SourceInfo> sources_;
};

} // namespace bethconv::archive
