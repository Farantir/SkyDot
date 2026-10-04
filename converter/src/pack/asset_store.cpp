// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/asset_store.hpp"

#include "bethconv/io/byte_writer.hpp"
#include "bethconv/io/span_reader.hpp"
#include "bethconv/io/span_stream.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <share.h>
#endif

namespace bethconv::pack {
namespace {

constexpr io::FourCC k_index_magic{"BCAI"};
constexpr const char* k_index_name = "assets.idx";

/// Large sequential writes: the point of the blob.
constexpr std::size_t k_write_buffer = 8u << 20;

[[nodiscard]] std::unexpected<io::ParseError> store_error(const std::filesystem::path& path,
                                                          io::ErrorKind kind, std::string detail) {
    return std::unexpected(io::ParseError{
        .origin = path.string(), .offset = 0, .kind = kind, .detail = std::move(detail)});
}

[[nodiscard]] std::unexpected<io::ParseError> fs_error(const std::filesystem::path& path,
                                                       std::string_view what,
                                                       const std::error_code& ec) {
    return store_error(path, io::ErrorKind::corrupt, std::string(what) + ": " + ec.message());
}

/// fopen with the native path type, so Windows does not narrow it to the
/// ANSI code page. Shared like fopen (_wfopen_s would lock readers out).
[[nodiscard]] std::FILE* open_file(const std::filesystem::path& path, const char* mode) {
#if defined(_WIN32)
    std::wstring wide_mode(mode, mode + std::char_traits<char>::length(mode));
    return _wfsopen(path.c_str(), wide_mode.c_str(), _SH_DENYNO);
#else
    return std::fopen(path.c_str(), mode);
#endif
}

[[nodiscard]] bool seek_to(std::FILE* file, std::uint64_t offset) {
#if defined(_WIN32)
    return _fseeki64(file, static_cast<long long>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

[[nodiscard]] std::uint64_t aligned(std::uint64_t value) {
    return (value + k_blob_alignment - 1) / k_blob_alignment * k_blob_alignment;
}

[[nodiscard]] bool write_all(std::FILE* file, std::span<const std::byte> bytes) {
    return bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
}

[[nodiscard]] bool write_zeros(std::FILE* file, std::uint64_t count) {
    static constexpr std::array<std::byte, k_blob_alignment> zeros{};
    return count == 0 || write_all(file, std::span(zeros).first(count));
}

/// Write `bytes` to `path` through a temporary file and a rename, so a reader
/// sees the old file or the new one.
[[nodiscard]] io::ParseResult<void> replace_file(const std::filesystem::path& path,
                                                 std::span<const std::byte> bytes) {
    auto temporary = path;
    temporary += ".tmp";
    std::string error;
    if (!io::write_file(temporary, bytes, error)) {
        return store_error(path, io::ErrorKind::corrupt, std::move(error));
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        return fs_error(path, "cannot replace", ec);
    }
    return {};
}

[[nodiscard]] io::ParseResult<AssetIndex> read_index(const std::filesystem::path& path) {
    auto file = io::MappedFile::open(path);
    if (!file) {
        return std::unexpected(file.error());
    }
    return AssetIndex::parse(file->bytes(), path.string());
}

/// Hex names of the loose assets under `root/assets`.
[[nodiscard]] io::ParseResult<std::unordered_set<std::string>>
scan_loose(const std::filesystem::path& root) {
    std::unordered_set<std::string> present;
    std::error_code ec;
    for (std::filesystem::directory_iterator bucket(root / "assets", ec), end;
         !ec && bucket != end; bucket.increment(ec)) {
        // An entry that cannot be statted is neither a bucket nor an asset.
        std::error_code kind;
        if (!bucket->is_directory(kind)) {
            continue;
        }
        std::error_code inner;
        for (std::filesystem::directory_iterator file(bucket->path(), inner), last;
             !inner && file != last; file.increment(inner)) {
            if (file->is_regular_file(kind)) {
                present.insert(file->path().stem().string());
            }
        }
    }
    if (ec) {
        return fs_error(root / "assets", "cannot read the existing pack", ec);
    }
    return present;
}

} // namespace

std::string_view to_string(StoreLayout layout) noexcept {
    return layout == StoreLayout::blob ? "blob" : "loose";
}

std::optional<StoreLayout> layout_from_string(std::string_view name) noexcept {
    if (name == "blob") {
        return StoreLayout::blob;
    }
    if (name == "loose") {
        return StoreLayout::loose;
    }
    return std::nullopt;
}

std::string blob_file_name(std::uint32_t generation) {
    return std::format("assets-{:04}.blob", generation);
}

// ---- AssetIndex -----------------------------------------------------------

io::ParseResult<AssetIndex> AssetIndex::parse(std::span<const std::byte> bytes,
                                              std::string_view origin) {
    io::SpanReader reader(bytes, origin);
    const auto bad = [&](std::string detail) {
        return std::unexpected(io::ParseError{.origin = std::string(origin),
                                              .offset = reader.position(),
                                              .kind = io::ErrorKind::corrupt,
                                              .detail = std::move(detail)});
    };

    const auto magic = reader.tag();
    if (!magic) {
        return std::unexpected(magic.error());
    }
    if (*magic != k_index_magic) {
        return std::unexpected(io::ParseError{.origin = std::string(origin),
                                              .offset = 0,
                                              .kind = io::ErrorKind::bad_magic,
                                              .detail = "not an asset index: '" +
                                                        magic->to_string() + "'"});
    }
    const auto version = reader.get<std::uint32_t>();
    const auto generation = reader.get<std::uint32_t>();
    const auto count = reader.get<std::uint32_t>();
    const auto blob_bytes = reader.get<std::uint64_t>();
    if (!version || !generation || !count || !blob_bytes) {
        return bad("truncated header");
    }
    if (*version != k_asset_index_version) {
        return std::unexpected(io::ParseError{
            .origin = std::string(origin),
            .offset = 4,
            .kind = io::ErrorKind::unsupported,
            .detail = "asset index v" + std::to_string(*version) + ", and this reads v" +
                      std::to_string(k_asset_index_version)});
    }
    if (*count > reader.remaining() / k_asset_index_entry) {
        return bad(std::to_string(*count) + " entries do not fit in " +
                   std::to_string(reader.remaining()) + " bytes");
    }

    AssetIndex index;
    index.generation = *generation;
    index.blob_bytes = *blob_bytes;
    index.entries.reserve(*count);
    for (std::uint32_t i = 0; i < *count; ++i) {
        BlobEntry entry;
        const auto hash = reader.bytes(entry.hash.bytes.size());
        const auto offset = reader.get<std::uint64_t>();
        const auto size = reader.get<std::uint64_t>();
        const auto kind = reader.get<std::uint8_t>();
        if (!hash || !offset || !size || !kind || !reader.skip(7)) {
            return bad("truncated entry " + std::to_string(i));
        }
        std::ranges::copy(*hash, entry.hash.bytes.begin());
        if (*kind > static_cast<std::uint8_t>(AssetKind::animation)) {
            return bad("entry " + std::to_string(i) + " has unknown kind " +
                       std::to_string(*kind));
        }
        if (*offset > index.blob_bytes || *size > index.blob_bytes - *offset) {
            return bad("entry " + std::to_string(i) + " lies outside the blob's " +
                       std::to_string(index.blob_bytes) + " bytes");
        }
        if (!index.entries.empty() && !(index.entries.back().hash.bytes < entry.hash.bytes)) {
            return bad("entries are not sorted by hash at " + std::to_string(i));
        }
        entry.offset = *offset;
        entry.size = *size;
        entry.kind = static_cast<AssetKind>(*kind);
        index.entries.push_back(entry);
    }
    if (reader.remaining() != 0) {
        return bad(std::to_string(reader.remaining()) + " bytes after the last entry");
    }
    return index;
}

std::vector<std::byte> AssetIndex::serialize() const {
    std::vector<const BlobEntry*> sorted;
    sorted.reserve(entries.size());
    for (const auto& entry : entries) {
        sorted.push_back(&entry);
    }
    std::ranges::sort(sorted, [](const BlobEntry* a, const BlobEntry* b) {
        return a->hash.bytes < b->hash.bytes;
    });

    io::ByteWriter out;
    out.reserve_more(k_asset_index_header + sorted.size() * k_asset_index_entry);
    out.put(k_index_magic.value);
    out.put(k_asset_index_version);
    out.put(generation);
    out.put(static_cast<std::uint32_t>(sorted.size()));
    out.put(blob_bytes);
    for (const auto* entry : sorted) {
        out.put_bytes(entry->hash.bytes);
        out.put(entry->offset);
        out.put(entry->size);
        out.put(static_cast<std::uint8_t>(entry->kind));
        out.put_bytes(std::array<std::byte, 7>{});
    }
    return out.take();
}

const BlobEntry* AssetIndex::find(const ContentHash& hash) const noexcept {
    const auto it = std::ranges::lower_bound(
        entries, hash.bytes, {}, [](const BlobEntry& entry) { return entry.hash.bytes; });
    return it != entries.end() && it->hash == hash ? &*it : nullptr;
}

// ---- AssetStore -----------------------------------------------------------

AssetStore::AssetStore() = default;

AssetStore::~AssetStore() {
    if (blob_ != nullptr) {
        std::fclose(blob_);
    }
}

AssetStore::AssetStore(AssetStore&& other) noexcept
    : root_(std::move(other.root_)), layout_(other.layout_), index_(std::move(other.index_)),
      by_hex_(std::move(other.by_hex_)), blob_(std::exchange(other.blob_, nullptr)),
      buffer_(std::move(other.buffer_)), present_(std::move(other.present_)) {}

AssetStore& AssetStore::operator=(AssetStore&& other) noexcept {
    if (this != &other) {
        if (blob_ != nullptr) {
            std::fclose(blob_);
        }
        root_ = std::move(other.root_);
        layout_ = other.layout_;
        index_ = std::move(other.index_);
        by_hex_ = std::move(other.by_hex_);
        blob_ = std::exchange(other.blob_, nullptr);
        buffer_ = std::move(other.buffer_);
        present_ = std::move(other.present_);
    }
    return *this;
}

io::ParseResult<AssetStore> AssetStore::open(const std::filesystem::path& root,
                                             StoreLayout layout) {
    AssetStore store;
    store.root_ = root;
    store.layout_ = layout;

    std::error_code ec;
    std::filesystem::create_directories(layout == StoreLayout::loose ? root / "assets" : root, ec);
    if (ec) {
        return fs_error(root, "cannot create the pack directory", ec);
    }

    if (layout == StoreLayout::loose) {
        auto present = scan_loose(root);
        if (!present) {
            return std::unexpected(present.error());
        }
        store.present_ = std::move(*present);
        return store;
    }

    const auto index_path = root / k_index_name;
    if (std::filesystem::exists(index_path, ec)) {
        auto index = read_index(index_path);
        if (!index) {
            return std::unexpected(index.error());
        }
        store.index_ = std::move(*index);
    } else {
        store.index_.generation = 1;
    }
    for (std::size_t i = 0; i < store.index_.entries.size(); ++i) {
        store.by_hex_.emplace(store.index_.entries[i].hash.hex(), i);
    }

    // Bytes past what the index covers are an interrupted run's: drop them.
    const auto blob_path = root / blob_file_name(store.index_.generation);
    if (!std::filesystem::exists(blob_path, ec)) {
        if (store.index_.blob_bytes != 0) {
            return store_error(blob_path, io::ErrorKind::corrupt,
                               "the asset index names this blob, which is missing");
        }
        std::string error;
        if (!io::write_file(blob_path, {}, error)) {
            return store_error(blob_path, io::ErrorKind::corrupt, std::move(error));
        }
    }
    const auto on_disk = std::filesystem::file_size(blob_path, ec);
    if (ec) {
        return fs_error(blob_path, "cannot stat", ec);
    }
    if (on_disk < store.index_.blob_bytes) {
        return store_error(blob_path, io::ErrorKind::truncated,
                           std::to_string(on_disk) + " bytes, and the index covers " +
                               std::to_string(store.index_.blob_bytes));
    }
    if (on_disk > store.index_.blob_bytes) {
        std::filesystem::resize_file(blob_path, store.index_.blob_bytes, ec);
        if (ec) {
            return fs_error(blob_path, "cannot drop an interrupted run's bytes", ec);
        }
    }

    store.blob_ = open_file(blob_path, "r+b");
    if (store.blob_ == nullptr) {
        return store_error(blob_path, io::ErrorKind::corrupt, "cannot open for appending");
    }
    store.buffer_ = std::make_unique<char[]>(k_write_buffer);
    std::setvbuf(store.blob_, store.buffer_.get(), _IOFBF, k_write_buffer);
    if (!seek_to(store.blob_, store.index_.blob_bytes)) {
        return store_error(blob_path, io::ErrorKind::corrupt, "cannot seek to the end");
    }
    return store;
}

bool AssetStore::contains(const ContentHash& hash) const {
    return layout_ == StoreLayout::blob ? by_hex_.contains(hash.hex())
                                        : present_.contains(hash.hex());
}

io::ParseResult<void> AssetStore::put(const ContentHash& hash, AssetKind kind,
                                      std::span<const std::byte> bytes) {
    std::string hex = hash.hex();
    if (layout_ == StoreLayout::loose) {
        const auto path = root_ / asset_relative_path(hex, kind);
        std::string error;
        if (!io::write_file(path, bytes, error)) {
            return store_error(path, io::ErrorKind::corrupt, std::move(error));
        }
        present_.insert(std::move(hex));
        return {};
    }

    const auto path = root_ / blob_file_name(index_.generation);
    const std::uint64_t offset = aligned(index_.blob_bytes);
    if (!write_zeros(blob_, offset - index_.blob_bytes) || !write_all(blob_, bytes)) {
        return store_error(path, io::ErrorKind::corrupt, "write failed (disk full?)");
    }
    index_.blob_bytes = offset + bytes.size();
    index_.entries.push_back(
        BlobEntry{.hash = hash, .offset = offset, .size = bytes.size(), .kind = kind});
    by_hex_.emplace(std::move(hex), index_.entries.size() - 1);
    return {};
}

io::ParseResult<void> AssetStore::write_index() {
    const auto path = root_ / blob_file_name(index_.generation);
    if (std::fflush(blob_) != 0) {
        return store_error(path, io::ErrorKind::corrupt, "flush failed (disk full?)");
    }
    return replace_file(root_ / k_index_name, index_.serialize());
}

io::ParseResult<void> AssetStore::compact(const std::unordered_set<std::string>& keep_hex,
                                          StoreStats& stats) {
    const auto old_path = root_ / blob_file_name(index_.generation);
    if (std::fflush(blob_) != 0) {
        return store_error(old_path, io::ErrorKind::corrupt, "flush failed (disk full?)");
    }

    AssetIndex next;
    next.generation = index_.generation + 1;
    const auto new_path = root_ / blob_file_name(next.generation);
    {
        auto old_blob = io::MappedFile::open(old_path);
        if (!old_blob && index_.blob_bytes != 0) {
            return std::unexpected(old_blob.error());
        }
        std::FILE* out = open_file(new_path, "wb");
        if (out == nullptr) {
            return store_error(new_path, io::ErrorKind::corrupt, "cannot create");
        }
        std::setvbuf(out, buffer_.get(), _IOFBF, k_write_buffer);

        // Offset order keeps the copy sequential on both sides.
        std::vector<const BlobEntry*> kept;
        for (const auto& entry : index_.entries) {
            if (keep_hex.contains(entry.hash.hex())) {
                kept.push_back(&entry);
            }
        }
        std::ranges::sort(kept, {}, &BlobEntry::offset);
        bool ok = true;
        for (const auto* entry : kept) {
            const std::uint64_t offset = aligned(next.blob_bytes);
            const auto bytes = old_blob->bytes().subspan(entry->offset, entry->size);
            if (!write_zeros(out, offset - next.blob_bytes) || !write_all(out, bytes)) {
                ok = false;
                break;
            }
            next.blob_bytes = offset + entry->size;
            next.entries.push_back(BlobEntry{
                .hash = entry->hash, .offset = offset, .size = entry->size, .kind = entry->kind});
        }
        ok = std::fclose(out) == 0 && ok;
        if (!ok) {
            std::error_code ignored;
            std::filesystem::remove(new_path, ignored);
            return store_error(new_path, io::ErrorKind::corrupt, "write failed (disk full?)");
        }
    }

    // The index switches to the new blob in one rename; only then does the
    // old one go.
    if (auto written = replace_file(root_ / k_index_name, next.serialize()); !written) {
        return written;
    }
    std::fclose(blob_);
    blob_ = nullptr;
    std::error_code ec;
    std::filesystem::remove(old_path, ec);

    stats.pruned = index_.entries.size() - next.entries.size();
    index_ = std::move(next);
    by_hex_.clear();
    for (std::size_t i = 0; i < index_.entries.size(); ++i) {
        by_hex_.emplace(index_.entries[i].hash.hex(), i);
    }
    blob_ = open_file(new_path, "r+b");
    if (blob_ == nullptr || !seek_to(blob_, index_.blob_bytes)) {
        return store_error(new_path, io::ErrorKind::corrupt, "cannot reopen for appending");
    }
    std::setvbuf(blob_, buffer_.get(), _IOFBF, k_write_buffer);
    return {};
}

io::ParseResult<StoreStats> AssetStore::finish(const std::unordered_set<std::string>& referenced_hex,
                                               bool prune) {
    StoreStats stats;
    if (layout_ == StoreLayout::loose) {
        std::error_code ec;
        for (std::filesystem::directory_iterator bucket(root_ / "assets", ec), end;
             !ec && bucket != end; bucket.increment(ec)) {
            std::error_code kind;
            if (!bucket->is_directory(kind)) {
                continue;
            }
            std::error_code inner;
            for (std::filesystem::directory_iterator file(bucket->path(), inner), last;
                 !inner && file != last; file.increment(inner)) {
                if (!file->is_regular_file(kind)) {
                    continue;
                }
                const auto hex = file->path().stem().string();
                if (referenced_hex.contains(hex)) {
                    stats.bytes += file->file_size(inner);
                    continue;
                }
                ++stats.orphaned;
                if (prune) {
                    std::error_code removed;
                    if (std::filesystem::remove(file->path(), removed)) {
                        ++stats.pruned;
                        present_.erase(hex);
                    }
                }
            }
        }
        return stats;
    }

    for (const auto& entry : index_.entries) {
        if (!referenced_hex.contains(entry.hash.hex())) {
            ++stats.orphaned;
        }
    }
    if (prune && stats.orphaned != 0) {
        if (auto compacted = compact(referenced_hex, stats); !compacted) {
            return std::unexpected(compacted.error());
        }
    } else if (auto written = write_index(); !written) {
        return std::unexpected(written.error());
    }
    stats.bytes = index_.blob_bytes;
    return stats;
}

// ---- AssetReader ----------------------------------------------------------

io::ParseResult<AssetReader> AssetReader::open(const std::filesystem::path& root) {
    AssetReader reader;
    reader.root_ = root;
    const auto index_path = root / k_index_name;
    std::error_code ec;
    if (!std::filesystem::exists(index_path, ec)) {
        reader.layout_ = StoreLayout::loose;
        return reader;
    }
    reader.layout_ = StoreLayout::blob;
    auto index = read_index(index_path);
    if (!index) {
        return std::unexpected(index.error());
    }
    reader.index_ = std::move(*index);
    if (reader.index_.blob_bytes == 0) {
        return reader;
    }
    const auto blob_path = root / blob_file_name(reader.index_.generation);
    auto blob = io::MappedFile::open(blob_path);
    if (!blob) {
        return std::unexpected(blob.error());
    }
    if (blob->size() < reader.index_.blob_bytes) {
        return store_error(blob_path, io::ErrorKind::truncated,
                           std::to_string(blob->size()) + " bytes, and the index covers " +
                               std::to_string(reader.index_.blob_bytes));
    }
    reader.blob_ = std::move(*blob);
    return reader;
}

io::ParseResult<AssetReader::Bytes> AssetReader::read(const VpathEntry& entry) const {
    if (layout_ == StoreLayout::loose) {
        auto file = io::MappedFile::open(root_ / std::filesystem::path(entry.asset_path()));
        if (!file) {
            return std::unexpected(file.error());
        }
        Bytes out;
        out.data = file->bytes();
        out.file = std::move(*file);
        out.data = out.file->bytes();
        return out;
    }
    const auto hash = parse_hex_hash(entry.hex);
    const BlobEntry* found = hash ? index_.find(*hash) : nullptr;
    if (found == nullptr) {
        return store_error(root_ / k_index_name, io::ErrorKind::bad_value,
                           "no asset " + entry.hex + " for " + entry.vpath);
    }
    if (found->size == 0) {
        return Bytes{};
    }
    return Bytes{.data = blob_->bytes().subspan(found->offset, found->size), .file = {}};
}

std::optional<std::filesystem::path> AssetReader::path_of(const VpathEntry& entry) const {
    if (layout_ == StoreLayout::blob) {
        return std::nullopt;
    }
    return root_ / std::filesystem::path(entry.asset_path());
}

} // namespace bethconv::pack
