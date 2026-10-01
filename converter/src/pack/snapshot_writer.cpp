// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/snapshot.hpp"

#include "bethconv/io/byte_writer.hpp"
#include "bethconv/record/field_walk.hpp"
#include "bethconv/record/types.hpp"

#include "bethconv/pack/records_generated.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace bethconv::pack {
namespace {

/// FNV-1a 64, as in the corpus harness. Detects changes and truncation, not
/// tampering; identical on every platform.
constexpr std::uint64_t k_fnv_offset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t k_fnv_prime = 0x100000001b3ULL;

void hash_into(std::uint64_t& hash, std::span<const std::byte> bytes) noexcept {
    for (const std::byte b : bytes) {
        hash ^= static_cast<std::uint8_t>(b);
        hash *= k_fnv_prime;
    }
}

/// Status bits (mirrored in `records.fbs` and the reader). One byte instead of
/// two bools to keep `Form` small.
constexpr std::uint8_t k_status_deleted = 0x01;
constexpr std::uint8_t k_status_injected = 0x02;

/// Grid key from `records.fbs`: two int32 packed into a sortable int64.
[[nodiscard]] constexpr std::int64_t grid_key(std::int32_t x, std::int32_t y) noexcept {
    return (static_cast<std::int64_t>(x) << 32) |
           static_cast<std::int64_t>(static_cast<std::uint32_t>(y));
}

/// Data gathered for one record during the walk.
struct Row {
    std::uint64_t payload_offset{};
    std::uint32_t id{};
    std::uint32_t type{};
    std::uint32_t parent{};
    std::uint32_t flags{};
    std::uint32_t payload_bytes{};
    std::uint32_t overrides{};
    std::uint16_t winner{};
    std::uint16_t owner{};
    std::uint8_t status{};
};

/// Pass two, writing as it goes. Payloads are appended to `out` immediately;
/// they are unbounded (527 MiB for vanilla SE), the index is not.
class WriteSink final : public record::MergedRecordSink {
public:
    WriteSink(std::ostream& out, SnapshotStats& stats) : out_(out), stats_(stats) {
        rows_.reserve(1u << 20);
    }

    void on_record(const record::MergedRecord& merged, const record::RecordContext& /*ctx*/,
                   io::SpanReader& data, const record::FormContext& /*form_ctx*/) override {
        // Copy the reader so reading the editor id does not consume it.
        io::SpanReader payload = data;
        const auto bytes = payload.bytes(payload.size());

        Row row{
            .payload_offset = blob_bytes_,
            .id = merged.form.value,
            .type = merged.type.value,
            .parent = merged.parent.value,
            .flags = merged.flags,
            .payload_bytes = 0,
            .overrides = merged.overrides,
            .winner = static_cast<std::uint16_t>(merged.winner),
            .owner = static_cast<std::uint16_t>(merged.owner),
            .status = static_cast<std::uint8_t>((merged.deleted ? k_status_deleted : 0U) |
                                                (merged.injected ? k_status_injected : 0U)),
        };

        if (bytes) {
            row.payload_bytes = static_cast<std::uint32_t>(bytes->size());
            hash_into(blob_hash_, *bytes);
            write_bytes(*bytes);
            blob_bytes_ += bytes->size();
        }
        rows_.push_back(row);

        read_names(merged, data);
    }

    [[nodiscard]] std::vector<Row>& rows() noexcept { return rows_; }
    [[nodiscard]] std::vector<std::pair<std::string, std::uint32_t>>& editor_ids() noexcept {
        return editor_ids_;
    }
    [[nodiscard]] std::map<std::uint32_t, std::vector<fb::GridCell>>& grids() noexcept {
        return grids_;
    }
    [[nodiscard]] std::uint64_t blob_bytes() const noexcept { return blob_bytes_; }
    [[nodiscard]] std::uint64_t blob_hash() const noexcept { return blob_hash_; }

private:
    void write_bytes(std::span<const std::byte> bytes) {
        // Writes bytes we own to a stream; no size comes from a file.
        out_.write(static_cast<const char*>(static_cast<const void*>(bytes.data())),
                   static_cast<std::streamsize>(bytes.size()));
    }

    /// Read EDID and an exterior CELL's XCLC by scanning the field list, without
    /// a record definition. So the editor-id index covers every record type.
    /// (EDID is a zstring everywhere; XCLC is 12 bytes on all 16,978 exterior
    /// cells.)
    void read_names(const record::MergedRecord& merged, io::SpanReader& data) {
        // A worldspace's persistent cell has XCLC (0, 0) like the real cell at
        // (0, 0), but holds the persistent references (15,197 in LE's Tamriel).
        // Only the header's persistent flag (0x400) tells them apart. It stays in
        // the snapshot but is left out of the grid index. Found by
        // `bethconv verify`; see docs/format-notes/snapshot.md.
        const bool is_cell = merged.type == io::FourCC{"CELL"} &&
                             !record::has_flag(merged.flags, record::RecordFlag::persistent);
        std::string editor_id;
        std::optional<fb::GridCell> grid;

        const auto walked = record::for_each_field(
            data, [&](const record::FieldHeader& field, io::SpanReader& body) {
                if (field.type == io::FourCC{"EDID"} && editor_id.empty()) {
                    if (const auto text = body.zstring()) {
                        editor_id = *text;
                    }
                } else if (is_cell && field.type == io::FourCC{"XCLC"} && !grid) {
                    const auto x = body.get<std::int32_t>();
                    const auto y = body.get<std::int32_t>();
                    if (x && y) {
                        grid = fb::GridCell(grid_key(*x, *y), merged.form.value);
                    }
                }
            });

        if (!walked) {
            // Fields do not tile: lose this editor id, count it.
            ++stats_.unwalkable;
            return;
        }
        if (!editor_id.empty()) {
            editor_ids_.emplace_back(std::move(editor_id), merged.form.value);
        }
        if (grid) {
            // An exterior cell's parent is its worldspace (innermost form-labelled
            // group). Interior cells have no XCLC.
            grids_[merged.parent.value].push_back(*grid);
        }
    }

    std::ostream& out_;
    SnapshotStats& stats_;
    std::vector<Row> rows_;
    std::vector<std::pair<std::string, std::uint32_t>> editor_ids_;
    std::map<std::uint32_t, std::vector<fb::GridCell>> grids_;
    std::uint64_t blob_bytes_ = 0;
    std::uint64_t blob_hash_ = k_fnv_offset;
};

/// The 64-byte header. Written first as a placeholder so the blob starts at a
/// known offset, then overwritten once sizes are known.
[[nodiscard]] std::vector<std::byte> render_header(const SnapshotHeader& header) {
    io::ByteWriter w;
    for (const char c : k_snapshot_magic) {
        w.put(static_cast<std::uint8_t>(c));
    }
    w.put(header.format_version);
    w.put(std::uint32_t{0}); ///< Reserved.
    w.put(header.fb_offset);
    w.put(header.fb_bytes);
    w.put(header.blob_offset);
    w.put(header.blob_bytes);
    w.put(header.form_count);
    w.put(header.blob_hash);
    auto bytes = std::move(w).take();
    bytes.resize(k_snapshot_header_size);
    return bytes;
}

[[nodiscard]] std::unexpected<io::ParseError> fail(const std::filesystem::path& path,
                                                   io::ErrorKind kind, std::string detail) {
    return std::unexpected(
        io::ParseError{.origin = path.string(), .offset = 0, .kind = kind, .detail = std::move(detail)});
}

} // namespace

io::ParseResult<SnapshotStats> write_snapshot(const record::MergedWorld& world,
                                              const record::LoadOrder& order,
                                              const std::filesystem::path& out,
                                              const SnapshotOptions& options) {
    SnapshotStats stats;

    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    if (!file) {
        return fail(out, io::ErrorKind::corrupt, "cannot open for writing");
    }

    // Placeholder header, overwritten at the end.
    const auto placeholder = render_header(SnapshotHeader{});
    file.write(static_cast<const char*>(static_cast<const void*>(placeholder.data())),
               static_cast<std::streamsize>(placeholder.size()));

    WriteSink sink(file, stats);
    world.for_each_record(sink);
    if (!file) {
        return fail(out, io::ErrorKind::corrupt, "write failed while streaming the payload blob");
    }

    auto& rows = sink.rows();
    // Sorted by FormID so `find` is a binary search.
    std::ranges::sort(rows, {}, &Row::id);

    flatbuffers::FlatBufferBuilder builder(1u << 20);

    std::vector<fb::Form> forms;
    forms.reserve(rows.size());
    std::map<std::uint32_t, std::vector<std::uint32_t>> by_type;
    std::map<std::uint32_t, std::vector<std::uint32_t>> by_parent;
    for (const auto& row : rows) {
        forms.emplace_back(row.payload_offset, row.id, row.type, row.parent, row.flags,
                           row.payload_bytes, row.overrides, row.winner, row.owner, row.status);
        by_type[row.type].push_back(row.id);
        if (row.parent != 0) {
            by_parent[row.parent].push_back(row.id);
        }
    }
    // Both index vectors are sorted: `rows` is sorted by id and push_back keeps
    // that order per bucket. Readers rely on it.
    const auto forms_off = builder.CreateVectorOfStructs(forms);

    auto& names = sink.editor_ids();
    std::ranges::sort(names);
    std::vector<flatbuffers::Offset<flatbuffers::String>> name_offsets;
    std::vector<std::uint32_t> name_forms;
    name_offsets.reserve(names.size());
    name_forms.reserve(names.size());
    for (const auto& [name, form] : names) {
        name_offsets.push_back(builder.CreateString(name));
        name_forms.push_back(form);
    }
    const auto names_off = builder.CreateVector(name_offsets);
    const auto name_forms_off = builder.CreateVector(name_forms);

    std::vector<flatbuffers::Offset<fb::TypeSpan>> type_spans;
    type_spans.reserve(by_type.size());
    for (const auto& [type, list] : by_type) {
        type_spans.push_back(fb::CreateTypeSpan(builder, type, builder.CreateVector(list)));
    }
    const auto types_off = builder.CreateVector(type_spans);

    std::vector<flatbuffers::Offset<fb::ChildList>> children;
    children.reserve(by_parent.size());
    for (const auto& [parent, list] : by_parent) {
        children.push_back(fb::CreateChildList(builder, parent, builder.CreateVector(list)));
    }
    const auto children_off = builder.CreateVector(children);

    std::vector<flatbuffers::Offset<fb::WorldGrid>> worlds;
    worlds.reserve(sink.grids().size());
    std::uint64_t grid_cells = 0;
    std::uint64_t grid_duplicates = 0;
    for (auto& [world_form, cells] : sink.grids()) {
        std::ranges::sort(cells, {}, [](const fb::GridCell& c) { return c.key(); });
        // Count cells sharing a square; the index can hold only one. Zero on
        // vanilla once persistent cells are excluded.
        for (std::size_t i = 1; i < cells.size(); ++i) {
            grid_duplicates += cells[i].key() == cells[i - 1].key() ? 1U : 0U;
        }
        grid_cells += cells.size();
        worlds.push_back(
            fb::CreateWorldGrid(builder, world_form, builder.CreateVectorOfStructs(cells)));
    }
    const auto worlds_off = builder.CreateVector(worlds);

    std::vector<flatbuffers::Offset<flatbuffers::String>> plugins;
    plugins.reserve(order.entries().size());
    for (const auto& entry : order.entries()) {
        plugins.push_back(builder.CreateString(entry.name));
    }
    const auto plugins_off = builder.CreateVector(plugins);

    const auto converter_off = builder.CreateString(options.converter);
    const auto language_off = builder.CreateString(options.language);

    const auto& merge = world.stats();
    fb::SnapshotBuilder root(builder);
    root.add_format_version(k_snapshot_format_version);
    root.add_converter(converter_off);
    root.add_language(language_off);
    root.add_plugins(plugins_off);
    root.add_forms(forms_off);
    root.add_editor_ids(names_off);
    root.add_editor_id_forms(name_forms_off);
    root.add_types(types_off);
    root.add_children(children_off);
    root.add_worlds(worlds_off);
    root.add_visited(merge.visited);
    root.add_collapsed(merge.collapsed);
    root.add_deleted(merge.deleted);
    root.add_injected(merge.injected);
    root.add_unresolved(merge.unresolved);
    fb::FinishSnapshotBuffer(builder, root.Finish());

    const std::uint64_t fb_offset = k_snapshot_header_size + sink.blob_bytes();
    file.write(static_cast<const char*>(static_cast<const void*>(builder.GetBufferPointer())),
               static_cast<std::streamsize>(builder.GetSize()));
    if (!file) {
        return fail(out, io::ErrorKind::corrupt, "write failed while writing the index");
    }

    const SnapshotHeader header{
        .format_version = k_snapshot_format_version,
        .fb_offset = fb_offset,
        .fb_bytes = builder.GetSize(),
        .blob_offset = k_snapshot_header_size,
        .blob_bytes = sink.blob_bytes(),
        .form_count = rows.size(),
        .blob_hash = sink.blob_hash(),
    };
    const auto rendered = render_header(header);
    file.seekp(0);
    file.write(static_cast<const char*>(static_cast<const void*>(rendered.data())),
               static_cast<std::streamsize>(rendered.size()));
    file.close();
    if (!file) {
        return fail(out, io::ErrorKind::corrupt, "write failed while finalizing the header");
    }

    stats.forms = rows.size();
    stats.payload_bytes = sink.blob_bytes();
    stats.editor_ids = names.size();
    stats.types = by_type.size();
    stats.children = by_parent.size();
    stats.worlds = worlds.size();
    stats.grid_cells = grid_cells;
    stats.grid_duplicates = grid_duplicates;
    stats.index_bytes = builder.GetSize();
    stats.file_bytes = fb_offset + builder.GetSize();
    return stats;
}

} // namespace bethconv::pack
