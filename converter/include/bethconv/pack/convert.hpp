// SPDX-License-Identifier: GPL-3.0-or-later
//
// `bethconv convert`: all passes over one mounted install, into one pack.
//
//   1. `records.fb`: merge and snapshot.
//   2. Every `.nif`, `.dds` and `.pex` in the mount, content-addressed into
//      `assets/` and deduplicated by source hash before conversion.
//   3. `manifest.json`, `vpath.idx`, `report.json`.
//
// Step 2 walks the mounted virtual filesystem, not the archives, so only
// winning files are included (vanilla SE: 172,914 instead of 174,431).
//
// A path whose source hash is already in the pack costs one hash and one index
// line; nothing is converted or written. Incremental rebuilds therefore scale
// with the change, not the install.
//
// The work list is sorted, so `--limit` runs are reproducible. Timings go to the
// progress callback, never into the pack.
#pragma once

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/mesh/gltf_writer.hpp"
#include "bethconv/mesh/nif_reader.hpp"
#include "bethconv/pack/pack_writer.hpp"
#include "bethconv/pack/snapshot.hpp"
#include "bethconv/pack/world.hpp"
#include "bethconv/record/load_order.hpp"
#include "bethconv/record/merge.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bethconv::pack {

struct ConvertOptions {
    std::filesystem::path out;

    /// Converter version; hashed into asset names and written to the manifest.
    std::string converter{"bethconv"};

    std::string language{std::string(record::k_default_language)};

    /// Write `records.fb`. Off for asset-only runs.
    bool write_records = true;

    bool convert_meshes = true;
    bool convert_textures = true;
    bool convert_scripts = true;
    /// Terrain and object LOD (.btr, .bto) as meshes; tree LOD and LOD settings
    /// (.btt, .lst, .lod) as LOD assets.
    bool convert_lod = true;

    mesh::ReadOptions mesh_read;

    /// Defaults to no glTF images (unlike mesh::WriteOptions): a pack's meshes
    /// name their textures in material extras, which the engine resolves through
    /// `vpath.idx`, and `view` adds images back (pack/pack_view.hpp). Part of
    /// the mesh fingerprint.
    mesh::WriteOptions mesh_write{.texture_refs = mesh::TextureRefs::none};

    /// Complete short DDS mip chains. Off is the control run. Part of the
    /// texture fingerprint.
    bool fix_mip_tail = true;

    /// Substring filter on the virtual path; empty means everything.
    std::string filter;

    /// Stop after this many inputs; 0 means no limit. Reproducible because the
    /// work list is sorted.
    std::size_t limit = 0;

    /// Also hash every archive into the manifest. Off by default (16 GiB for
    /// vanilla SE, and nothing checks it yet). Plugins are always hashed.
    bool hash_archives = false;

    bool prune_orphans = false;

    /// Where asset bytes go: one blob (default) or a file per asset.
    StoreLayout layout = StoreLayout::blob;

    /// Called every `progress_interval` inputs and at the end of each phase.
    /// Terminal output only.
    std::function<void(const std::string& phase, std::uint64_t done,
                       std::uint64_t total)> progress;
    std::uint64_t progress_interval = 2000;

    /// Fingerprints hashed into asset names, built explicitly from the options
    /// above. A new option that affects output must be added here, or rebuilds
    /// will reuse stale assets.
    [[nodiscard]] std::string mesh_settings() const;
    [[nodiscard]] std::string texture_settings() const;
    [[nodiscard]] std::string script_settings() const;
    [[nodiscard]] std::string lod_settings() const;
};

/// Results of `convert`.
struct ConvertResult {
    PackStats pack;
    std::optional<SnapshotStats> snapshot;
    std::optional<WorldStats> world;
    std::optional<record::MergeStats> merge;

    std::uint64_t sources{};      ///< Mounted archives and loose directories.
    std::uint64_t unique_paths{}; ///< Distinct virtual paths in the mount.
    std::uint64_t considered{};   ///< Paths passing the filter and limit.

    /// The first few failures, for the terminal; `report.json` has all.
    std::vector<PackFailure> first_failures;
};

/// Run every pass over `set` and `order` into `options.out`. The caller owns the
/// mount and the load order.
[[nodiscard]] io::ParseResult<ConvertResult> convert(const archive::ArchiveSet& set,
                                                     const record::LoadOrder& order,
                                                     const ConvertOptions& options);

} // namespace bethconv::pack
