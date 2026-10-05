// SPDX-License-Identifier: GPL-3.0-or-later
//
// A pack's `vpath.idx` and asset bytes, without Godot objects, so the loader
// threads can share it (formats/pack-format.md, "vpath.idx" and "Asset
// store"). Immutable after `open`; every query is safe from any thread.
//
// The blob is memory-mapped; a read copies one asset's bytes out of the
// mapping, straight into the buffer the caller supplies. Loose packs read the
// asset's file into it.
#pragma once

#include "assets/mapped_file.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace skydot {

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

    /// Where `read_into` puts an asset's bytes: given their count, returns room
    /// for that many (it may be null if the count is 0).
    using Allocate = std::function<std::uint8_t*(std::size_t)>;
    /// Copy the asset's bytes into the room `allocate` makes for them, once.
    /// False if the path is absent, its kind unknown or its file unreadable
    /// (`allocate` may have been called; the caller drops what it made).
    [[nodiscard]] bool read_into(const std::string& normalized_vpath, const Allocate& allocate) const;

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
