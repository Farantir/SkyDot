// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writes the pack directory:
//
//     pack/
//       manifest.json         version, converter, load order, source hashes
//       records.fb            record snapshot (written by pack/snapshot)
//       world.fb              cells, references, base objects (pack/world)
//       assets.idx, assets-<gen>.blob         assets, content-addressed (pack/asset_store)
//       (or assets/<bb>/<hash>.<ext> with the loose layout)
//       vpath.idx             virtual path -> content hash
//       report.json           every skipped or failed input, with reason
//
// Handles layout, dedupe and bookkeeping only; pack/convert.cpp runs the
// conversions. That split lets unit tests build a complete pack from synthetic
// files.
//
// No `.ktx2`: DDS is passed through (docs/spikes/headless-bake.md).
//
// Output is deterministic (same input, byte-identical pack): no wall-clock
// times anywhere, and `vpath.idx` and all `report.json` arrays are sorted
// rather than written in hash-table order.
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/pack/asset_store.hpp"
#include "bethconv/pack/content_hash.hpp"
#include "bethconv/pack/vpath_index.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace bethconv::pack {

// `AssetKind`, `k_pack_format_version` and asset paths are defined in
// vpath_index.hpp.

/// An input that did not become an asset. Never dropped silently.
struct PackFailure {
    std::string vpath;
    std::string stage;  ///< "read", "mesh", "texture", "script", "write".
    std::string kind;   ///< io::ErrorKind name, for counting.
    std::string detail;
};

/// A converted file with a warning (vanilla: 24 of 39,263 NIFs, all an
/// undecoded `bhkNiTriStripsShape`).
struct PackWarning {
    std::string vpath;
    std::string detail;
};

/// Inputs not converted by this version, counted by extension.
///
/// FaceGen, trees and `.btr`/`.bto` terrain LOD are not converted yet; counting
/// every unconverted extension lists them without guessing categories. Only
/// files are counted, not bytes: sizing them would mean reading ~100,000 files.
///
/// SpeedTree trees are `.nif` files and currently convert as regular meshes;
/// they cannot be told apart by path.
struct DeferredKind {
    std::uint64_t files{};
};

/// One source the pack was built from.
struct SourceRecord {
    std::string name;
    std::string kind;              ///< archive::to_string(SourceKind) or "plugin".
    std::uint64_t bytes{};
    std::optional<ContentHash> hash; ///< Always for plugins; optional for archives.
};

/// Summary of `records.fb` for the manifest, so a consumer can check it without
/// opening it.
struct RecordsRecord {
    std::uint64_t forms{};
    std::uint64_t file_bytes{};
    ContentHash hash;
};

/// Manifest fields this layer cannot determine itself.
/// Summary of `world.fb` for the manifest.
struct WorldRecord {
    std::uint64_t cells{};
    std::uint64_t refs{};
    std::uint64_t bases{};
    std::uint64_t file_bytes{};
    ContentHash hash;
};

/// What the pack was converted from, so a front end can convert it again with
/// the same inputs. Local paths: packs are never redistributed. Not hashed into
/// anything; the sources are (`source_hashes`).
struct InputRecord {
    std::string kind;          ///< "data" (a Data folder) or "mo2" (Data plus a profile).
    std::string edition;       ///< install::to_string(Edition).
    std::string data;          ///< The game's Data folder.
    std::string plugin_list;   ///< The list file used; empty if none.
    std::string mo2_instance;  ///< "mo2" only.
    std::string mo2_profile;   ///< "mo2" only.
    std::uint64_t mods{};      ///< "mo2" only: enabled mods mounted.
};

/// How textures were converted, so an engine can tell a reduced pack.
struct TextureRecord {
    std::uint32_t max_size{};   ///< 0: full size.
    bool complete_mip_chains{};
};

struct PackManifest {
    std::string converter;   ///< "bethconv 0.0.1". Also hashed into every asset.
    std::string language;
    std::optional<InputRecord> input;
    std::optional<TextureRecord> textures;
    std::vector<std::string> load_order;
    std::vector<SourceRecord> sources;
    std::optional<RecordsRecord> records;
    std::optional<WorldRecord> world;
};

struct PackOptions {
    /// Converter version, hashed into every asset name.
    std::string converter{"bethconv"};

    /// Per-kind settings fingerprints, hashed into asset names. Filled by
    /// convert.hpp.
    std::string mesh_settings;
    std::string texture_settings;
    std::string script_settings;
    std::string lod_settings;

    /// Where asset bytes go. Blob unless asked otherwise.
    StoreLayout layout = StoreLayout::blob;

    /// Delete assets not named by this run's index (for the blob, compact it).
    /// Off by default: partial rebuilds are legitimate and the directory
    /// belongs to the user. `finish()` counts orphans either way.
    bool prune_orphans = false;
};

/// Statistics from `finish()`.
struct PackStats {
    std::uint64_t inputs{};   ///< Virtual paths offered to the pack.
    std::uint64_t converted{}; ///< Assets actually written.
    std::uint64_t deduped{};   ///< Inputs whose asset already existed.
    std::uint64_t failed{};
    std::uint64_t deferred{};  ///< Inputs of a kind this pass does not convert.
    std::uint64_t deferred_kinds{}; ///< Distinct extensions among them.

    std::uint64_t meshes{};
    std::uint64_t textures{};
    std::uint64_t scripts{};
    std::uint64_t lod{};

    std::uint64_t asset_bytes{};  ///< Written by this run.
    std::uint64_t store_bytes{};  ///< Every stored asset, after pruning.
    std::uint64_t source_bytes{}; ///< Read from the mount, converted or not.

    /// Bytes not written because the asset already existed.
    std::uint64_t dedupe_saved_bytes{};

    std::uint64_t distinct_assets{};
    std::uint64_t index_entries{};

    /// Stored assets not named by the index. 0 for a fresh directory.
    std::uint64_t orphaned_assets{};
    std::uint64_t pruned_assets{};

    std::uint64_t warnings{};
    std::uint64_t manifest_bytes{};
    std::uint64_t index_bytes{};
    std::uint64_t report_bytes{};
};

/// A reserved asset for one input. The hash comes from the source bytes, so
/// `already_present` is known before conversion; if true, do not convert.
struct AssetSlot {
    ContentHash hash;
    AssetKind kind{};
    bool already_present{};

    /// Used by `store`/`reuse` to write the index entry. `reserve` does not,
    /// so a failed conversion leaves no index entry for a missing asset.
    std::string vpath;
    std::string source;
    std::uint64_t source_bytes{};
};

/// Builds a pack directory. Not copyable; one per output tree.
class PackWriter {
public:
    PackWriter() = default;
    ~PackWriter();
    PackWriter(const PackWriter&) = delete;
    PackWriter& operator=(const PackWriter&) = delete;
    PackWriter(PackWriter&&) noexcept;
    PackWriter& operator=(PackWriter&&) noexcept;

    /// Create or reopen `root`. When reopening, existing assets are indexed so
    /// they are not converted again.
    [[nodiscard]] static io::ParseResult<PackWriter> create(const std::filesystem::path& root,
                                                            PackOptions options = {});

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    /// Path for `records.fb`.
    [[nodiscard]] std::filesystem::path records_path() const;
    /// Path for `world.fb`.
    [[nodiscard]] std::filesystem::path world_path() const;

    /// Hash `source` and check whether its asset already exists. The index
    /// entry is written later by `store` or `reuse`.
    [[nodiscard]] AssetSlot reserve(std::string_view vpath, AssetKind kind,
                                    std::span<const std::byte> source,
                                    std::string_view source_name);

    /// Write converted bytes for a slot `reserve` reported as absent.
    [[nodiscard]] io::ParseResult<void> store(const AssetSlot& slot,
                                              std::span<const std::byte> converted);

    /// Record an `already_present` slot. Call for every one; the dedupe numbers
    /// depend on it.
    void reuse(const AssetSlot& slot);

    void fail(PackFailure failure);
    void warn(PackWarning warning);

    /// An input of an unconverted kind. `extension` includes the dot and is
    /// lowercase.
    void defer(std::string_view extension);

    /// Write manifest.json, vpath.idx and report.json and finish the store. The
    /// writer stays usable, so tests can call it twice and compare.
    [[nodiscard]] io::ParseResult<PackStats> finish(const PackManifest& manifest);

    [[nodiscard]] const std::vector<PackFailure>& failures() const noexcept { return failures_; }
    [[nodiscard]] const std::vector<PackWarning>& warnings() const noexcept { return warnings_; }
    [[nodiscard]] const std::map<std::string, DeferredKind>& deferred() const noexcept {
        return deferred_;
    }

private:
    struct IndexEntry {
        std::string vpath;
        std::string hex;
        AssetKind kind{};
        std::string source; ///< Mounted source that won the path.
    };

    /// Add `slot`'s path to the index (from `store` and `reuse`).
    void record(const AssetSlot& slot);

    [[nodiscard]] std::string settings_for(AssetKind kind) const;

    std::filesystem::path root_;
    PackOptions options_;

    std::vector<IndexEntry> index_;
    AssetStore store_;
    std::vector<PackFailure> failures_;
    std::vector<PackWarning> warnings_;
    std::map<std::string, DeferredKind> deferred_;

    PackStats stats_;
};

} // namespace bethconv::pack
