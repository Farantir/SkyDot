// SPDX-License-Identifier: GPL-3.0-or-later
//
// The record snapshot (`records.fb`): the merged world written to disk and
// read back.
//
//     [ 64-byte header ][ payload blob ][ FlatBuffer ]
//
// The FlatBuffer holds identity, provenance and the indices; the blob holds the
// winning records' field bytes. The blob comes first because it is streamed
// during the merge's second pass, while the index can only be built at the end.
//
// FlatBuffers use 32-bit offsets (2 GiB max) and vanilla SE's payloads alone are
// 527 MiB, so payloads live in the blob with 64-bit offsets and only the index
// (about 55 bytes per form) must fit.
//
// Reading is untrusted input: the file may be truncated, edited or from another
// version. The header and blob slices go through `io::SpanReader`; the
// FlatBuffer goes through `flatbuffers::Verifier` before any accessor is used.
#pragma once

#include "bethconv/io/mapped_file.hpp"
#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/load_order.hpp"
#include "bethconv/record/merge.hpp"
#include "bethconv/record/types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::pack {

/// "BETHSNAP". The FlatBuffer's own identifier `BSN1` sits 64 bytes later; the
/// outer magic names the container, the inner one the schema.
inline constexpr std::string_view k_snapshot_magic = "BETHSNAP";

/// Bumped when the meaning of `records.fbs` or this header changes (see
/// formats/pack-format.md). A mismatch is refused with a clear message.
inline constexpr std::uint32_t k_snapshot_format_version = 1;

/// Container header, little-endian, in write order. Fixed at 64 bytes with a
/// reserved tail, so later versions can add fields without moving existing
/// ones.
struct SnapshotHeader {
    std::uint32_t format_version{};
    std::uint64_t fb_offset{};
    std::uint64_t fb_bytes{};
    std::uint64_t blob_offset{};
    std::uint64_t blob_bytes{};
    std::uint64_t form_count{};
    /// FNV-1a over the blob. Detects truncation, not tampering (the FlatBuffer
    /// is not covered). Checked by `verify --deep`, not by `open`.
    std::uint64_t blob_hash{};
};

inline constexpr std::size_t k_snapshot_header_size = 64;

// ---- writing --------------------------------------------------------------

struct SnapshotOptions {
    /// Converter version, recorded as provenance.
    std::string converter{"bethconv"};
    /// The `.STRINGS` language used during the merge. Snapshots in different
    /// languages differ.
    std::string language{std::string(record::k_default_language)};
};

/// Write statistics.
struct SnapshotStats {
    std::uint64_t forms{};
    std::uint64_t payload_bytes{};
    std::uint64_t editor_ids{};
    std::uint64_t types{};
    std::uint64_t children{};
    std::uint64_t worlds{};
    std::uint64_t grid_cells{};

    /// Exterior cells sharing a grid square with another, per worldspace. The
    /// grid index keeps one cell per square, so duplicates would make lookups
    /// return the wrong cell. Zero on all vanilla installs once persistent cells
    /// are excluded.
    std::uint64_t grid_duplicates{};

    std::uint64_t index_bytes{}; ///< The FlatBuffer only.
    std::uint64_t file_bytes{};

    /// Records whose fields do not walk, so no editor id could be read. Not an
    /// error; a rising count would point at the merge passing bad payloads.
    std::uint64_t unwalkable{};
};

/// Write `world` to `out`, replacing any existing file.
///
/// Runs the merge's second pass (one walk of the load order) and streams
/// payloads to disk instead of holding them in memory.
[[nodiscard]] io::ParseResult<SnapshotStats> write_snapshot(
    const record::MergedWorld& world, const record::LoadOrder& order,
    const std::filesystem::path& out, const SnapshotOptions& options = {});

// ---- reading --------------------------------------------------------------

/// One form. A view: `payload` points into the mapping.
struct FormView {
    record::FormId id;
    io::FourCC type;
    record::FormId parent;
    std::uint32_t flags{};
    std::uint32_t overrides{};
    std::uint16_t winner{};
    std::uint16_t owner{};
    bool deleted{};
    bool injected{};
    std::span<const std::byte> payload;
};

/// A mapped and verified `records.fb`. Read-only and zero-copy: no allocation
/// per lookup, payload spans point into the mapping. The engine wraps this.
class Snapshot {
public:
    Snapshot() = default;
    ~Snapshot();

    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Snapshot(Snapshot&&) noexcept;
    Snapshot& operator=(Snapshot&&) noexcept;

    /// Map and verify. Fails instead of returning an unusable Snapshot.
    [[nodiscard]] static io::ParseResult<Snapshot> open(const std::filesystem::path& path);

    /// Same, over caller-owned bytes (unit tests, fuzzing). `bytes` must outlive
    /// the Snapshot.
    [[nodiscard]] static io::ParseResult<Snapshot> from_bytes(std::span<const std::byte> bytes,
                                                              std::string_view origin);

    [[nodiscard]] std::uint32_t format_version() const noexcept;
    [[nodiscard]] std::string_view converter() const noexcept;
    [[nodiscard]] std::string_view language() const noexcept;
    /// The load order this world was built from, in order.
    [[nodiscard]] std::vector<std::string_view> plugins() const;

    [[nodiscard]] std::size_t size() const noexcept; ///< Forms.
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    /// Binary search over the sorted forms; nullopt if not defined.
    [[nodiscard]] std::optional<FormView> find(record::FormId id) const;

    /// By index in 0..size(), in FormID order.
    [[nodiscard]] std::optional<FormView> at(std::size_t index) const;

    /// Editor id -> FormID, binary search over sorted names.
    [[nodiscard]] std::optional<record::FormId> find_editor_id(std::string_view name) const;

    /// Editor id of a form, or empty. A linear scan (the reverse direction is
    /// not indexed); for diagnostics only.
    [[nodiscard]] std::string_view editor_id_of(record::FormId id) const;

    /// All forms of one type, sorted. Empty if the type does not occur.
    [[nodiscard]] std::span<const std::uint32_t> of_type(io::FourCC type) const;

    /// Forms whose parent is `parent`, sorted; for a CELL, its contents.
    [[nodiscard]] std::span<const std::uint32_t> children_of(record::FormId parent) const;

    /// The exterior cell at grid (x, y) of `world`, if there is one.
    [[nodiscard]] std::optional<record::FormId> cell_at(record::FormId world, std::int32_t x,
                                                        std::int32_t y) const;

    /// Worldspaces that have a grid index, in FormID order.
    [[nodiscard]] std::vector<record::FormId> worlds() const;

    /// Merge statistics recorded at write time.
    [[nodiscard]] record::MergeStats merge_stats() const noexcept;

    /// Re-hash the blob and compare with the header (not done by `open`).
    [[nodiscard]] bool blob_intact() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bethconv::pack
