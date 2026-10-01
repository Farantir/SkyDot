// SPDX-License-Identifier: GPL-3.0-or-later
//
// A pack's `vpath.idx` and asset bytes, without Godot objects, so the loader
// threads can share it (formats/pack-format.md, "vpath.idx" and "Asset
// store"). Immutable after `open`; every query is safe from any thread.
//
// The blob is memory-mapped; a read copies one asset's bytes out of the
// mapping. Loose packs read the asset's file.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

class PackStore {
public:
    struct Entry {
        std::string hash;   ///< 64 lowercase hex characters.
        std::string kind;   ///< As written; never empty.
        std::string source; ///< Archive or folder that won this path.
    };

    /// Asset kinds this engine reads, and their stored extension.
    static std::string_view extension_of(std::string_view kind);

    /// Parse `vpath.idx` text. False with `error` set on a malformed line.
    bool read_index(std::string_view text, std::string_view header, std::string& error);

    /// Attach the blob layout: `assets.idx` bytes and the blob's path.
    bool open_blob(std::span<const std::uint8_t> index, const std::string& blob_path,
                   std::string& error);
    /// Attach the loose layout under `pack_dir` (native path).
    void open_loose(const std::string& pack_dir);

    [[nodiscard]] const Entry* find(const std::string& normalized_vpath) const;
    [[nodiscard]] std::size_t size() const { return index_.size(); }
    [[nodiscard]] std::size_t unknown_kinds() const { return unknown_kinds_; }
    [[nodiscard]] bool is_blob() const { return blob_ != nullptr; }

    /// The asset's bytes, or nullopt if the path is absent, its kind unknown,
    /// or its file unreadable.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> read(const std::string& normalized_vpath) const;

    /// The loose file for a path; "" for the blob layout.
    [[nodiscard]] std::string loose_path(const Entry& entry) const;

private:
    struct Span {
        std::uint64_t offset{};
        std::uint64_t size{};
    };

    std::unordered_map<std::string, Entry> index_;
    std::size_t unknown_kinds_{0};

    std::unique_ptr<MappedFile> blob_;
    std::unordered_map<std::string, Span> blob_entries_; ///< By hex hash.
    std::string loose_dir_;
};

} // namespace skydot
