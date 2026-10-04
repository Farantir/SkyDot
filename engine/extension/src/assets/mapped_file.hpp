// SPDX-License-Identifier: GPL-3.0-or-later
//
// A read-only memory mapping of a whole file: the pack's asset blob and its
// world.fb. The file must not shrink while it is mapped.
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace skydot {

/// A read-only memory mapping of a whole file.
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    /// Map `path` (UTF-8, native). False with `error` set on failure. An empty
    /// file maps to an empty span.
    bool open(const std::string& path, std::string& error);
    [[nodiscard]] std::span<const std::uint8_t> bytes() const { return {data_, size_}; }

private:
    const std::uint8_t* data_{nullptr};
    std::size_t size_{0};
#if defined(_WIN32)
    void* file_{nullptr};
    void* mapping_{nullptr};
#endif
};

} // namespace skydot
