// SPDX-License-Identifier: GPL-3.0-or-later
//
// Read-only memory mapping. Skyrim.esm alone is 250 MB, so files are never read
// into heap buffers.
#pragma once

#include "bethconv/io/span_reader.hpp"

#include <filesystem>
#include <span>
#include <string>

namespace bethconv::io {

/// RAII read-only mmap. Move-only.
///
/// If another process truncates the file while mapped, reads raise SIGBUS;
/// SpanReader cannot prevent that. Acceptable for an offline converter running
/// in its own process over the user's files.
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    /// Maps the whole file. An empty file maps to an empty span, not an error.
    [[nodiscard]] static ParseResult<MappedFile> open(const std::filesystem::path& path);

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] const std::string& origin() const noexcept { return origin_; }
    [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr || !data_.empty(); }

    /// A reader over the whole mapping. The reader borrows; keep this alive.
    [[nodiscard]] SpanReader reader() const noexcept { return SpanReader(data_, origin_); }

private:
    void reset() noexcept;

    std::span<const std::byte> data_{};
    std::string origin_;
    void* handle_{nullptr}; ///< Platform mapping handle.
};

} // namespace bethconv::io
