// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/snapshot.hpp"

#include "bethconv/io/byte_view.hpp"
#include "bethconv/pack/records_generated.h"

#include <flatbuffers/verifier.h>

#include <algorithm>
#include <bit>
#include <memory>
#include <string>
#include <utility>

namespace bethconv::pack {
namespace {

constexpr std::uint64_t k_fnv_offset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t k_fnv_prime = 0x100000001b3ULL;

constexpr std::uint8_t k_status_deleted = 0x01;
constexpr std::uint8_t k_status_injected = 0x02;

[[nodiscard]] constexpr std::int64_t grid_key(std::int32_t x, std::int32_t y) noexcept {
    return (static_cast<std::int64_t>(x) << 32) |
           static_cast<std::int64_t>(static_cast<std::uint32_t>(y));
}

/// The zero-copy spans below bypass FlatBuffers' byte-swapping accessors, so
/// they are only correct on little-endian hosts. Every CI and Godot target is
/// little-endian; this makes a big-endian port fail to build instead of reading
/// swapped FormIDs.
static_assert(std::endian::native == std::endian::little,
              "the pack format's zero-copy vectors assume a little-endian host");

} // namespace

/// FlatBuffers details, hidden so users of snapshot.hpp need no FlatBuffers
/// dependency.
class Snapshot::Impl {
public:
    io::MappedFile mapping;                 ///< Empty when bytes are borrowed.
    std::span<const std::byte> bytes;
    std::string origin;
    SnapshotHeader header;
    const fb::Snapshot* root{nullptr};
    std::span<const std::byte> blob;

    /// Map both sections and verify the FlatBuffer. A static member so it can
    /// name the private `Impl`.
    [[nodiscard]] static io::ParseResult<std::unique_ptr<Impl>> build(
        std::span<const std::byte> bytes, std::string_view origin);

    [[nodiscard]] std::optional<FormView> view(const fb::Form& form) const {
        FormView out{
            .id = record::FormId{form.id()},
            .type = io::FourCC{form.type()},
            .parent = record::FormId{form.parent()},
            .flags = form.flags(),
            .overrides = form.overrides(),
            .winner = form.winner(),
            .owner = form.owner(),
            .deleted = (form.status() & k_status_deleted) != 0,
            .injected = (form.status() & k_status_injected) != 0,
            .payload = {},
        };
        // The verifier covers the index, not the blob, so blob slices go
        // through SpanReader.
        io::SpanReader reader(blob, origin);
        if (const auto slice = reader.subreader_at(form.payload_offset(), form.payload_bytes())) {
            if (const auto payload = io::SpanReader(*slice).bytes(form.payload_bytes())) {
                out.payload = *payload;
            } else {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
        return out;
    }
};

Snapshot::~Snapshot() = default;
Snapshot::Snapshot(Snapshot&&) noexcept = default;
Snapshot& Snapshot::operator=(Snapshot&&) noexcept = default;

namespace {

/// Read the container header and check every field against the file length
/// before using it.
[[nodiscard]] io::ParseResult<SnapshotHeader> read_header(io::SpanReader& reader,
                                                          std::size_t file_bytes) {
    const auto magic = reader.chars(k_snapshot_magic.size());
    if (!magic) {
        return std::unexpected(magic.error());
    }
    if (*magic != k_snapshot_magic) {
        return reader.fail(io::ErrorKind::bad_magic, "not a bethconv record snapshot");
    }

    SnapshotHeader header;
    const auto version = reader.get<std::uint32_t>();
    if (!version) {
        return std::unexpected(version.error());
    }
    header.format_version = *version;
    if (header.format_version != k_snapshot_format_version) {
        // Version mismatch: clear error, never a misparse.
        return reader.fail(io::ErrorKind::unsupported,
                           "pack format v" + std::to_string(header.format_version) +
                               ", this build reads v" +
                               std::to_string(k_snapshot_format_version));
    }
    if (const auto reserved = reader.get<std::uint32_t>(); !reserved) {
        return std::unexpected(reserved.error());
    }

    const auto field = [&reader](std::uint64_t& out) -> io::ParseResult<void> {
        const auto value = reader.get<std::uint64_t>();
        if (!value) {
            return std::unexpected(value.error());
        }
        out = *value;
        return {};
    };
    for (std::uint64_t* slot : {&header.fb_offset, &header.fb_bytes, &header.blob_offset,
                                &header.blob_bytes, &header.form_count, &header.blob_hash}) {
        if (const auto ok = field(*slot); !ok) {
            return std::unexpected(ok.error());
        }
    }

    const auto within = [file_bytes](std::uint64_t offset, std::uint64_t size) {
        return offset <= file_bytes && size <= file_bytes - offset;
    };
    if (!within(header.blob_offset, header.blob_bytes)) {
        return reader.fail(io::ErrorKind::out_of_range,
                           "payload blob runs past the end of the file");
    }
    if (!within(header.fb_offset, header.fb_bytes)) {
        return reader.fail(io::ErrorKind::out_of_range, "index runs past the end of the file");
    }
    if (header.fb_bytes == 0) {
        return reader.fail(io::ErrorKind::corrupt, "index is empty");
    }
    // FlatBuffers reads fields in place; a misaligned index is undefined
    // behaviour. Snapshots written before the padding existed have one.
    if (header.fb_offset % k_snapshot_index_alignment != 0) {
        return reader.fail(io::ErrorKind::unsupported,
                           "index is not 8-byte aligned (written by an older converter); "
                           "convert again");
    }
    return header;
}

} // namespace

io::ParseResult<std::unique_ptr<Snapshot::Impl>> Snapshot::Impl::build(
    std::span<const std::byte> bytes, std::string_view origin) {
    auto impl = std::make_unique<Snapshot::Impl>();
    impl->bytes = bytes;
    impl->origin = std::string(origin);

    io::SpanReader reader(bytes, origin);
    auto header = read_header(reader, bytes.size());
    if (!header) {
        return std::unexpected(std::move(header).error());
    }
    impl->header = *header;

    io::SpanReader blob_reader(bytes, origin);
    auto blob = blob_reader.subreader_at(header->blob_offset, header->blob_bytes);
    if (!blob) {
        return std::unexpected(std::move(blob).error());
    }
    if (const auto blob_bytes = io::SpanReader(*blob).bytes(header->blob_bytes)) {
        impl->blob = *blob_bytes;
    } else {
        return std::unexpected(blob_bytes.error());
    }

    io::SpanReader index_reader(bytes, origin);
    auto index = index_reader.subreader_at(header->fb_offset, header->fb_bytes);
    if (!index) {
        return std::unexpected(std::move(index).error());
    }
    const auto index_bytes = io::SpanReader(*index).bytes(header->fb_bytes);
    if (!index_bytes) {
        return std::unexpected(index_bytes.error());
    }

    // Every FlatBuffers accessor follows offsets from the file, so the whole
    // buffer (offsets, vector lengths, strings) is verified before any
    // accessor is called.
    const auto index_u8 = io::as_u8(*index_bytes);
    flatbuffers::Verifier verifier(index_u8.data(), index_u8.size());
    if (!fb::VerifySnapshotBuffer(verifier)) {
        return index_reader.fail(io::ErrorKind::corrupt,
                                 "the index did not verify as a records.fb");
    }

    impl->root = fb::GetSnapshot(index_bytes->data());
    if (impl->root == nullptr || impl->root->forms() == nullptr) {
        return index_reader.fail(io::ErrorKind::corrupt, "the index has no form table");
    }
    if (impl->root->forms()->size() != header->form_count) {
    // The form count is stored twice; a mismatch means a hand-edited header or
    // an interrupted write.
        return index_reader.fail(io::ErrorKind::corrupt,
                                 "header says " + std::to_string(header->form_count) +
                                     " forms, index holds " +
                                     std::to_string(impl->root->forms()->size()));
    }
    return impl;
}

io::ParseResult<Snapshot> Snapshot::open(const std::filesystem::path& path) {
    auto mapping = io::MappedFile::open(path);
    if (!mapping) {
        return std::unexpected(std::move(mapping).error());
    }
    auto impl = Impl::build(mapping->bytes(), mapping->origin());
    if (!impl) {
        return std::unexpected(std::move(impl).error());
    }
    Snapshot out;
    (*impl)->mapping = std::move(*mapping);
    // Point at the mapping now owned by the Impl; the local was moved from.
    (*impl)->bytes = (*impl)->mapping.bytes();
    out.impl_ = std::move(*impl);
    return out;
}

io::ParseResult<Snapshot> Snapshot::from_bytes(std::span<const std::byte> bytes,
                                               std::string_view origin) {
    auto impl = Impl::build(bytes, origin);
    if (!impl) {
        return std::unexpected(std::move(impl).error());
    }
    Snapshot out;
    out.impl_ = std::move(*impl);
    return out;
}

std::uint32_t Snapshot::format_version() const noexcept {
    return impl_ ? impl_->header.format_version : 0;
}

// Accessors store each FlatBuffers pointer in a local: calling a table
// accessor twice means two loads the compiler must treat as possibly
// different, and GCC -O2 warns about a possible null dereference.
std::string_view Snapshot::converter() const noexcept {
    if (!impl_) {
        return {};
    }
    const auto* text = impl_->root->converter();
    return text == nullptr ? std::string_view{} : text->string_view();
}

std::string_view Snapshot::language() const noexcept {
    if (!impl_) {
        return {};
    }
    const auto* text = impl_->root->language();
    return text == nullptr ? std::string_view{} : text->string_view();
}

std::vector<std::string_view> Snapshot::plugins() const {
    std::vector<std::string_view> out;
    if (!impl_) {
        return out;
    }
    const auto* names = impl_->root->plugins();
    if (names == nullptr) {
        return out;
    }
    out.reserve(names->size());
    for (const auto* name : *names) {
        out.push_back(name->string_view());
    }
    return out;
}

std::size_t Snapshot::size() const noexcept {
    if (!impl_) {
        return 0;
    }
    const auto* forms = impl_->root->forms();
    return forms == nullptr ? 0 : forms->size();
}

std::optional<FormView> Snapshot::at(std::size_t index) const {
    if (!impl_) {
        return std::nullopt;
    }
    const auto* forms = impl_->root->forms();
    if (forms == nullptr || index >= forms->size()) {
        return std::nullopt;
    }
    return impl_->view(*forms->Get(static_cast<flatbuffers::uoffset_t>(index)));
}

std::optional<FormView> Snapshot::find(record::FormId id) const {
    if (!impl_) {
        return std::nullopt;
    }
    // `forms` is sorted by its (key), id; the writer sorts it (see records.fbs
    // and test_snapshot.cpp).
    const auto* forms = impl_->root->forms();
    const auto* form = forms != nullptr ? forms->LookupByKey(id.value) : nullptr;
    if (form == nullptr) {
        return std::nullopt;
    }
    return impl_->view(*form);
}

std::optional<record::FormId> Snapshot::find_editor_id(std::string_view name) const {
    if (!impl_) {
        return std::nullopt;
    }
    const auto* names = impl_->root->editor_ids();
    const auto* forms = impl_->root->editor_id_forms();
    if (names == nullptr || forms == nullptr || names->size() != forms->size()) {
        return std::nullopt;
    }
    std::size_t low = 0;
    std::size_t high = names->size();
    while (low < high) {
        const std::size_t mid = low + (high - low) / 2;
        const auto value = names->Get(static_cast<flatbuffers::uoffset_t>(mid))->string_view();
        if (value == name) {
            return record::FormId{forms->Get(static_cast<flatbuffers::uoffset_t>(mid))};
        }
        if (value < name) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return std::nullopt;
}

std::string_view Snapshot::editor_id_of(record::FormId id) const {
    if (!impl_) {
        return {};
    }
    const auto* names = impl_->root->editor_ids();
    const auto* forms = impl_->root->editor_id_forms();
    if (names == nullptr || forms == nullptr || names->size() != forms->size()) {
        return {};
    }
    for (flatbuffers::uoffset_t i = 0; i < forms->size(); ++i) {
        if (forms->Get(i) == id.value) {
            return names->Get(i)->string_view();
        }
    }
    return {};
}

std::span<const std::uint32_t> Snapshot::of_type(io::FourCC type) const {
    if (!impl_) {
        return {};
    }
    const auto* types = impl_->root->types();
    const auto* span = types != nullptr ? types->LookupByKey(type.value) : nullptr;
    if (span == nullptr || span->forms() == nullptr) {
        return {};
    }
    return {span->forms()->data(), span->forms()->size()};
}

std::span<const std::uint32_t> Snapshot::children_of(record::FormId parent) const {
    if (!impl_) {
        return {};
    }
    const auto* lists = impl_->root->children();
    const auto* list = lists != nullptr ? lists->LookupByKey(parent.value) : nullptr;
    if (list == nullptr || list->children() == nullptr) {
        return {};
    }
    return {list->children()->data(), list->children()->size()};
}

std::optional<record::FormId> Snapshot::cell_at(record::FormId world, std::int32_t x,
                                                std::int32_t y) const {
    if (!impl_) {
        return std::nullopt;
    }
    const auto* worlds = impl_->root->worlds();
    if (worlds == nullptr) {
        return std::nullopt;
    }
    const auto* grid = worlds->LookupByKey(world.value);
    if (grid == nullptr || grid->cells() == nullptr) {
        return std::nullopt;
    }

    // Two cells can share a square (the writer counts them), so GridCell.key
    // is no `(key)` in records.fbs and this search stays by hand.
    const auto key = grid_key(x, y);
    const auto* cells = grid->cells();
    std::size_t low = 0;
    std::size_t high = cells->size();
    while (low < high) {
        const std::size_t mid = low + (high - low) / 2;
        const auto* cell = cells->Get(static_cast<flatbuffers::uoffset_t>(mid));
        if (cell->key() == key) {
            return record::FormId{cell->cell()};
        }
        if (cell->key() < key) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return std::nullopt;
}

std::vector<record::FormId> Snapshot::worlds() const {
    std::vector<record::FormId> out;
    if (!impl_) {
        return out;
    }
    const auto* worlds = impl_->root->worlds();
    if (worlds == nullptr) {
        return out;
    }
    out.reserve(worlds->size());
    for (const auto* grid : *worlds) {
        out.push_back(record::FormId{grid->world()});
    }
    return out;
}

record::MergeStats Snapshot::merge_stats() const noexcept {
    record::MergeStats stats;
    if (!impl_) {
        return stats;
    }
    const auto* plugin_names = impl_->root->plugins();
    stats.plugins = plugin_names == nullptr ? 0 : plugin_names->size();
    stats.visited = impl_->root->visited();
    stats.forms = size();
    stats.collapsed = impl_->root->collapsed();
    stats.deleted = impl_->root->deleted();
    stats.injected = impl_->root->injected();
    stats.unresolved = impl_->root->unresolved();
    return stats;
}

bool Snapshot::blob_intact() const {
    if (!impl_) {
        return false;
    }
    std::uint64_t hash = k_fnv_offset;
    for (const std::byte b : impl_->blob) {
        hash ^= static_cast<std::uint8_t>(b);
        hash *= k_fnv_prime;
    }
    return hash == impl_->header.blob_hash;
}

} // namespace bethconv::pack
