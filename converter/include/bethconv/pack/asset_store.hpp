// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where a pack keeps its asset bytes. Two layouts:
//
//     blob (default)
//       assets.idx              hash -> (offset, size, kind), sorted by hash
//       assets-<gen>.blob       every asset, appended in conversion order
//
//     loose
//       assets/<bb>/<hash><ext> one file per asset, two-level fanout
//
// The blob exists because tens of thousands of small files overwhelm slow
// targets: an SMR disk behind ntfs-3g hung the whole mount on a full convert
// (formats/pack-format.md, "Asset store"). A blob is written sequentially and
// is one file to open, map and copy.
//
// Appending is crash-safe: `assets.idx` is replaced atomically at the end and
// records how many blob bytes it covers, so bytes past that are leftovers of an
// interrupted run and get overwritten. Compaction (`--prune`) writes the next
// generation's blob before switching the index to it.
#pragma once

#include "bethconv/io/mapped_file.hpp"
#include "bethconv/io/parse_error.hpp"
#include "bethconv/pack/content_hash.hpp"
#include "bethconv/pack/vpath_index.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bethconv::pack {

enum class StoreLayout : std::uint8_t { blob, loose };

[[nodiscard]] std::string_view to_string(StoreLayout layout) noexcept;
[[nodiscard]] std::optional<StoreLayout> layout_from_string(std::string_view name) noexcept;

/// `assets.idx`, little-endian:
///
///     magic "BCAI", u32 version, u32 generation, u32 count, u64 blob_bytes
///     count x { u8 hash[32], u64 offset, u64 size, u8 kind, u8 pad[7] }
///
/// Entries are sorted by hash; the blob is `assets-<generation, 4 digits>.blob`.
inline constexpr std::uint32_t k_asset_index_version = 1;
inline constexpr std::size_t k_asset_index_header = 24;
inline constexpr std::size_t k_asset_index_entry = 56;

/// Blob entries start on this boundary.
inline constexpr std::uint64_t k_blob_alignment = 16;

struct BlobEntry {
    ContentHash hash;
    std::uint64_t offset{};
    std::uint64_t size{};
    AssetKind kind{};
};

/// A parsed `assets.idx`.
struct AssetIndex {
    std::uint32_t generation{};
    std::uint64_t blob_bytes{};
    std::vector<BlobEntry> entries; ///< Sorted by hash.

    [[nodiscard]] static io::ParseResult<AssetIndex> parse(std::span<const std::byte> bytes,
                                                           std::string_view origin);
    [[nodiscard]] std::vector<std::byte> serialize() const;

    /// Binary search; null if absent.
    [[nodiscard]] const BlobEntry* find(const ContentHash& hash) const noexcept;
};

[[nodiscard]] std::string blob_file_name(std::uint32_t generation);

/// What `AssetStore::finish` found and did.
struct StoreStats {
    std::uint64_t orphaned{}; ///< Stored assets no index entry names.
    std::uint64_t pruned{};   ///< Of those, removed.
    std::uint64_t bytes{};    ///< Blob size, or the loose files' total.
};

/// Writes assets. One per pack directory.
class AssetStore {
public:
    AssetStore();
    ~AssetStore();
    AssetStore(const AssetStore&) = delete;
    AssetStore& operator=(const AssetStore&) = delete;
    AssetStore(AssetStore&&) noexcept;
    AssetStore& operator=(AssetStore&&) noexcept;

    /// Open or create the store under `root`. Assets of the other layout in
    /// the same directory are ignored, not converted: a switch reconverts.
    [[nodiscard]] static io::ParseResult<AssetStore> open(const std::filesystem::path& root,
                                                          StoreLayout layout);

    [[nodiscard]] StoreLayout layout() const noexcept { return layout_; }
    [[nodiscard]] std::uint32_t generation() const noexcept { return index_.generation; }
    [[nodiscard]] bool contains(const ContentHash& hash) const;

    [[nodiscard]] io::ParseResult<void> put(const ContentHash& hash, AssetKind kind,
                                            std::span<const std::byte> bytes);

    /// Flush, count assets not in `referenced`, remove them if `prune`, and
    /// write the index. Callable more than once.
    [[nodiscard]] io::ParseResult<StoreStats>
    finish(const std::unordered_set<std::string>& referenced_hex, bool prune);

private:
    [[nodiscard]] io::ParseResult<void> write_index();
    [[nodiscard]] io::ParseResult<void> compact(const std::unordered_set<std::string>& keep_hex,
                                                StoreStats& stats);

    std::filesystem::path root_;
    StoreLayout layout_{StoreLayout::blob};

    // blob
    AssetIndex index_;
    std::unordered_map<std::string, std::size_t> by_hex_; ///< Into index_.entries.
    std::FILE* blob_{nullptr};
    std::unique_ptr<char[]> buffer_;

    // loose
    std::unordered_set<std::string> present_; ///< Hex names on disk.
};

/// Reads assets of either layout.
class AssetReader {
public:
    /// The blob layout if `assets.idx` exists, else loose.
    [[nodiscard]] static io::ParseResult<AssetReader> open(const std::filesystem::path& root);

    [[nodiscard]] StoreLayout layout() const noexcept { return layout_; }

    /// The asset's bytes, valid while the reader lives (blob) or the returned
    /// mapping lives (loose).
    struct Bytes {
        std::span<const std::byte> data;
        std::optional<io::MappedFile> file; ///< Loose only.
    };
    [[nodiscard]] io::ParseResult<Bytes> read(const VpathEntry& entry) const;

    /// A loose asset's path; nullopt for the blob layout.
    [[nodiscard]] std::optional<std::filesystem::path> path_of(const VpathEntry& entry) const;

private:
    std::filesystem::path root_;
    StoreLayout layout_{StoreLayout::loose};
    AssetIndex index_;
    std::optional<io::MappedFile> blob_;
};

} // namespace bethconv::pack
