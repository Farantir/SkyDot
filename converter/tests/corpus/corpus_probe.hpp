// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs the converter over a real install and compares the results with
// committed numbers (counts, versions, hashes, known-bad paths), never game
// data.
//
// Shared by the test (compares with `corpus-expectations.json`) and
// `bethconv-corpus-snapshot` (regenerates it), so the two cannot drift.
#pragma once

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/mesh/gltf_writer.hpp"
#include "bethconv/mesh/nif_reader.hpp"
#include "bethconv/pack/convert.hpp"
#include "bethconv/pack/pack_view.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/histogram.hpp"
#include "bethconv/record/load_order.hpp"
#include "bethconv/record/merge.hpp"
#include "bethconv/record/plugin.hpp"
#include "bethconv/texture/dds.hpp"
#include "bethconv/texture/mip_tail.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::corpus {

/// Facts about one plugin: counts, flags and hashes only.
struct PluginFacts {
    std::string file;
    /// Size on disk. Unlike every other field it only changes when the data
    /// does, so a mismatch identifies a game patch rather than a reader change.
    std::uint64_t file_bytes{};
    float header_version{};
    std::int32_t hedr_record_count{}; ///< HEDR's figure: records + groups.
    std::uint64_t records{};
    std::uint64_t groups{};
    std::uint64_t compressed_records{};
    std::uint64_t errors{};
    std::uint64_t record_types{};
    std::uint64_t masters{};
    bool is_master{};
    bool is_light{};
    bool is_localized{};
    /// FNV-1a over "TYPE count" for the type histogram; changes if any type's
    /// count changes.
    std::uint64_t type_histogram_hash{};
    /// Records whose fields tiled their payload, in basis points (an exact
    /// integer instead of a float).
    std::uint64_t field_coverage_bp{};
};

/// Facts about one mounted archive.
struct ArchiveFacts {
    std::string file;
    std::string kind;
    std::uint32_t version{};
    std::uint64_t files{};
};

/// Facts about a whole mounted set.
struct ArchiveScan {
    std::vector<ArchiveFacts> archives;
    std::uint64_t unique_paths{};
    std::uint64_t shadowed{};
    /// Archives listed in the expectations but missing on disk. An environment
    /// problem, reported separately from mount errors.
    std::vector<std::string> missing;
};

/// A plugin's place in a resolved load order. `index` is text ("00",
/// "FE:002") so diffs are readable and the two index spaces stay distinct.
struct PlacedPlugin {
    std::string name;
    std::string index;
    std::string flags; ///< "M" master, "L" light, "S" localized; "-" for absent.
    std::uint64_t masters{};
};

/// A resolved load order: counts and placements only.
struct LoadOrderFacts {
    std::vector<PlacedPlugin> plugins;
    std::uint64_t normal_used{};
    std::uint64_t light_used{};
    std::uint64_t problems{};
};

/// Facts about one texture before and after the texture pass. A full sweep is
/// too slow for ctest (57 s for SE under ASan), so a few files covering every
/// branch are pinned, including the exact output bytes.
struct TextureFacts {
    std::string vpath;
    std::string format;  ///< "DXT1", "RGB32", "BC7".
    std::string kind;    ///< "2d", "cubemap", "volume".
    std::string outcome; ///< Decision of the mip-tail fix.
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t faces{};
    std::uint32_t stored_levels{};
    std::uint32_t full_chain_levels{};
    std::uint64_t declared_bytes{};
    /// Size of the pass's output: the completed file, or the unchanged source.
    std::uint64_t output_bytes{};
    /// FNV-1a over the output; changes if any byte of tail or header changes.
    std::uint64_t output_hash{};
};

/// Facts about one converted mesh, both sides. A full sweep is too slow for
/// ctest (454 s LE, 603 s SE under ASan), so a few files covering every branch
/// are pinned. IR counts catch a reader dropping shapes; `glb_hash` catches any
/// byte change in the writer's output (e.g. the old texture-URI bug).
struct MeshFacts {
    std::string vpath;
    std::string flavor;      ///< "le" or "se", from the stream version.
    std::string nif_version; ///< As stated in the file.
    std::uint32_t nif_stream{};

    std::uint64_t nodes{};
    std::uint64_t roots{};
    std::uint64_t primitives{};
    std::uint64_t materials{};
    std::uint64_t skins{};
    std::uint64_t collision{};
    std::uint64_t joints{};    ///< Across all skins.
    std::uint64_t vertices{};  ///< Across all primitives.
    std::uint64_t triangles{};
    std::uint64_t warnings{};

    /// Attribute presence summed over primitives, so a dropped TANGENT or
    /// COLOR_0 changes a number.
    std::uint64_t with_normals{};
    std::uint64_t with_tangents{};
    std::uint64_t with_uvs{};
    std::uint64_t with_colors{};
    std::uint64_t with_joints{};

    std::uint64_t glb_bytes{};
    std::uint64_t glb_hash{};

    /// Image URIs and how many contain a percent escape. Makes the
    /// control-byte case visible (67 vanilla SE FaceGen meshes reference
    /// `textures\<0x08>NOR`, written as `textures/%08nor`); `glb_hash` would
    /// change too, but without saying why.
    std::uint64_t images{};
    std::uint64_t escaped_uris{};

    /// Checked, never recorded; see `GlbCheck`.
    std::uint64_t glb_control_bytes{};
    bool glb_json_parses{};
};

/// Facts about one emitted GLB's JSON chunk. Both checks came from a consumer
/// rejecting a file the harness had passed:
///
///   * No byte below 0x20: Blender rejects the GLB ("Invalid control
///     character"); 67 vanilla SE meshes had one until `0186824`.
///   * Parses with a strict reader: fastgltf 0.9.0 writes non-finite floats as
///     out-of-range numbers without an error (`greybeardstatic.nif`).
///
/// Both hold for every GLB, so every GLB the harness converts is checked.
struct GlbCheck {
    bool container_ok{};    ///< Magic, version, first chunk tagged JSON.
    bool json_parses{};     ///< nlohmann parsed it (exceptions enabled).
    std::uint64_t control_bytes{}; ///< Bytes below 0x20 in the JSON chunk.
    std::uint64_t images{};
    std::uint64_t escaped_uris{};
};

/// Read a GLB's JSON chunk and reduce it to these facts. Never throws;
/// unparseable JSON gives `json_parses` false.
[[nodiscard]] GlbCheck inspect_glb(std::span<const std::byte> glb);

/// Facts about one form after merging. The editor id and name are hashed so no
/// game text is committed; this also allows pinning names from `.STRINGS`
/// tables.
struct MergedFormFacts {
    std::string form;   ///< Global FormID as text: "0x00027D1C".
    std::string type;
    std::string parent; ///< Global, or "-" at top level.
    std::string winner; ///< Filename of the winning plugin.
    std::string owner;  ///< Filename of the plugin owning the FormID space.
    std::uint32_t overrides{};
    std::uint32_t flags{};
    bool deleted{};
    bool injected{};

    /// Whether the second pass reached this form. Deleted forms are still
    /// indexed, so `present` and `deleted` together distinguish dropped from
    /// deleted.
    bool present{};

    /// The winning payload as the second pass delivers it (inflated). Changes
    /// when a different plugin wins, which the totals would not show.
    std::uint64_t payload_bytes{};
    std::uint64_t payload_hash{};

    /// FNV-1a of the parsed EDID and FULL, if the type has a definition; 0
    /// otherwise.
    std::uint64_t editor_id_hash{};
    std::uint64_t name_hash{};
    /// The name came from a `.STRINGS` table (index -> winner's table -> text).
    bool name_from_table{};
};

/// Facts about a merged load order. There is no cheap subset (the merge walks
/// the whole order), so this is the expensive pin; see tests/corpus/README.md.
struct MergeFacts {
    std::uint64_t plugins{};
    std::uint64_t visited{};
    std::uint64_t forms{};
    std::uint64_t collapsed{};
    std::uint64_t deleted{};
    std::uint64_t injected{};
    std::uint64_t unresolved{};
    std::uint64_t unparented{};
    std::uint64_t errors{};
    std::uint64_t unreadable{};
    std::uint64_t string_tables{};
    std::uint64_t problems{};

    /// Records the second pass delivered. Must equal `forms`; a wrong winner
    /// choice keeps the form count plausible but breaks this equality.
    std::uint64_t forwarded{};

    /// FNV-1a over "TYPE count" of the merged world; changes if the collapse
    /// changes.
    std::uint64_t type_counts_hash{};

    std::vector<MergedFormFacts> forms_pinned;

    /// `records.fb` and `world.fb` written from this world into the probe's
    /// scratch directory and opened again. Each is pinned by its counts and
    /// an FNV-1a over its bytes (both writers are deterministic). Empty if
    /// writing or reopening failed.
    std::vector<std::pair<std::string, std::uint64_t>> snapshot;
    std::vector<std::pair<std::string, std::uint64_t>> world;

    /// One candidate form per class the merge can produce, printed by the
    /// snapshot tool when an install has no `forms` list yet. Not compared.
    std::vector<std::string> suggestions;
};

/// Facts about the field definitions over a whole install.
///
/// Must be zero: `failed`, `leftover` (a definition reading less than the field
/// holds, which no total would show) and `unreadable`. `unhandled` (fields no
/// definition claims) is recorded as a count and as `unhandled_hash`, so a
/// definition that stops consuming a field shows up even if the count does not
/// move.
struct FormsFacts {
    std::uint64_t plugins{};    ///< Plugins walked.
    std::uint64_t unreadable{}; ///< Listed, present, and failed to open.
    std::uint64_t localized{};  ///< Of those, how many are localized.

    /// Plugins whose `.STRINGS` tables were found, and their entry count. Zero
    /// tables on a localized install means the archive was not mounted and
    /// "0 unresolved" means nothing.
    std::uint64_t string_tables{};
    std::uint64_t table_entries{};

    std::uint64_t types_defined{}; ///< `defined_types().size()`.
    std::uint64_t types_seen{};    ///< How many of those occur in this install.

    std::uint64_t parsed{};
    std::uint64_t failed{};

    /// Occurrences and the distinct (type, field) pairs behind them.
    std::uint64_t leftover{};
    std::uint64_t leftover_fields{};
    std::uint64_t unhandled{};
    std::uint64_t unhandled_fields{};

    /// Names resolved from a table, and indices that resolved to nothing. The
    /// second is recorded, not asserted zero: it depends on which archive
    /// provides each table (see docs/format-notes/localized-strings.md).
    std::uint64_t resolved_strings{};
    std::uint64_t unresolved_strings{};

    /// FNV-1a over "TYPE seen parsed failed" per defined type. Counts records a
    /// definition ran on, unlike the per-plugin histograms.
    std::uint64_t type_counts_hash{};

    /// FNV-1a over "TYPE FIELD count" for unclaimed fields.
    std::uint64_t unhandled_hash{};
};

/// Every `.pex` in the mounted archives, decoded completely and written as a
/// script asset. `failed` must be zero; `asset_hash` (FNV-1a over the assets
/// in path order) moves with any change to the decoder or the asset format.
struct ScriptFacts {
    std::uint64_t scripts{};
    std::uint64_t failed{};
    std::uint64_t objects{};
    std::uint64_t functions{};
    std::uint64_t natives{};
    std::uint64_t instructions{};
    std::uint64_t asset_hash{};
};

/// Every `.hkx` in the mounted archives, decoded and written as an animation
/// asset. `failed` must be zero; `asset_hash` (FNV-1a over the assets in path
/// order) moves with any change to the reader or the asset format.
struct AnimationFacts {
    std::uint64_t files{};
    std::uint64_t failed{};
    /// Havok binary tagfiles, a container not read yet (6 in SE's Creation
    /// Club fishing content). Counted apart so `failed` stays zero.
    std::uint64_t tagfiles{};
    std::uint64_t skeletons{};
    std::uint64_t clips{};
    std::uint64_t frames{};
    std::uint64_t annotations{};
    /// Behaviour characters and their clip generators.
    std::uint64_t characters{};
    std::uint64_t clip_generators{};
    /// The animationdata text files (project, boundanims, SE's single file):
    /// files read, files that failed, clips and motions in them (the single
    /// file's projects included).
    std::uint64_t data_files{};
    std::uint64_t data_failed{};
    std::uint64_t data_clips{};
    std::uint64_t data_motions{};
    std::uint64_t asset_hash{};
};

/// Facts about one `bethconv convert` over a real install.
///
/// A full convert is too slow for ctest (55 s release, minutes under ASan), so
/// a filtered run is pinned and the filter stored with the results.
/// `inputs` and `considered` confirm the subset is unchanged.
///
/// `index_hash` changes when any asset hash changes (converter or fingerprint
/// change); `report_hash` covers the warning text.
struct ConvertFacts {
    std::string filter;
    std::uint64_t limit{};

    /// Virtual paths in the mount and those passing filter and limit. They
    /// change when the game is patched; check them first when things go red.
    std::uint64_t unique_paths{};
    std::uint64_t considered{};

    std::uint64_t inputs{};
    std::uint64_t converted{};
    std::uint64_t deduped{};
    std::uint64_t deferred{};
    std::uint64_t deferred_kinds{};
    std::uint64_t failed{};
    std::uint64_t warnings{};

    std::uint64_t meshes{};
    std::uint64_t textures{};
    std::uint64_t scripts{};
    std::uint64_t lod{};

    std::uint64_t distinct_assets{};
    std::uint64_t index_entries{};
    std::uint64_t orphaned_assets{};

    std::uint64_t asset_bytes{};
    std::uint64_t source_bytes{};
    std::uint64_t dedupe_saved_bytes{};

    /// FNV-1a over the three bookkeeping files. `manifest_hash` covers the
    /// plugin BLAKE3s, so like `file_bytes` it only changes with the data.
    std::uint64_t manifest_hash{};
    std::uint64_t index_hash{};
    std::uint64_t report_hash{};

    /// GLBs checked; nonzero so the zero-count assertions cannot pass
    /// vacuously.
    std::uint64_t glb_checked{};
    std::uint64_t glb_bad_container{};
    std::uint64_t glb_control_bytes{};
    std::uint64_t glb_unparseable{};
    std::uint64_t glb_escaped_uris{};
};

/// Facts about one `bethconv view` of the pack above. The view reads the pack
/// and rewrites GLBs, so it is a second consumer and a second writer.
///
/// `failed` must be zero. `image_refs_dangling` is recorded: on a filtered
/// pack most dangling references are textures the filter excluded, so it
/// measures whether escaping and unescaping agree, not Bethesda's 572 missing
/// textures (a full-install figure `bethconv view` prints).
struct ViewFacts {
    std::string filter;
    std::uint64_t limit{};

    std::uint64_t index_entries{};
    std::uint64_t considered{};
    std::uint64_t meshes{};
    std::uint64_t textures{};
    std::uint64_t scripts{};
    std::uint64_t lod{};

    std::uint64_t linked{};
    std::uint64_t copied{};
    std::uint64_t written{};
    std::uint64_t mesh_links{};
    std::uint64_t pulled_in{};

    std::uint64_t image_refs{};
    std::uint64_t image_refs_resolved{};
    std::uint64_t image_refs_dangling{};

    std::uint64_t failed{};
    std::uint64_t bytes{};

    /// The same GLB checks on what the view wrote, since it re-serializes the
    /// JSON.
    std::uint64_t glb_checked{};
    std::uint64_t glb_bad_container{};
    std::uint64_t glb_control_bytes{};
    std::uint64_t glb_unparseable{};
};

/// Read an environment variable; empty counts as unset (so `SKYRIM_DATA_SE=`
/// skips the install).
[[nodiscard]] inline std::optional<std::string> env(const char* name) {
#ifdef _MSC_VER
    char* buffer = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) {
        return std::nullopt;
    }
    std::string value(buffer);
    std::free(buffer);
#else
    const char* raw = std::getenv(name);
    if (raw == nullptr) {
        return std::nullopt;
    }
    const std::string value(raw);
#endif
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

/// TES4 header version as a two-decimal string ("0.94"), avoiding float noise
/// in the committed file.
[[nodiscard]] inline std::string format_version(float version) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(version));
    return std::string(buffer);
}

/// FNV-1a 64. A regression fingerprint, not cryptographic; identical on every
/// platform.
[[nodiscard]] inline std::uint64_t fnv1a(std::string_view bytes) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : bytes) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

/// The same hash over raw bytes.
[[nodiscard]] inline std::uint64_t fnv1a(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const std::byte b : bytes) {
        hash ^= static_cast<std::uint8_t>(b);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

/// Walk one plugin and reduce it to facts. Nullopt if it will not open (the
/// caller reports that as a failure).
[[nodiscard]] std::optional<PluginFacts> probe_plugin(const std::filesystem::path& path);

/// Mount `archives` (relative to `data_dir`, in order, equal priority so order
/// decides) into `set` and summarize. `set` is an out-parameter because callers
/// keep reading from it.
[[nodiscard]] ArchiveScan probe_archives(const std::filesystem::path& data_dir,
                                         const std::vector<std::string>& archives,
                                         archive::ArchiveSet& set);

/// Result of reading known-bad paths. Absent paths are kept apart from failed
/// ones, so a typo in the expectations cannot pass as a corrupt entry.
struct ReadCheck {
    std::vector<std::string> unresolved; ///< No source has this path.
    std::vector<std::string> failed;     ///< Present, extraction failed.
    std::vector<std::string> succeeded;  ///< Present and readable.
};

[[nodiscard]] ReadCheck check_reads(const archive::ArchiveSet& set,
                                    const std::vector<std::string>& vpaths);

/// Read one texture from `set` and run the texture pass. Nullopt if the path is
/// not in this install (a skip); a present but unparseable file gives
/// `outcome` "parse-error".
[[nodiscard]] std::optional<TextureFacts> probe_texture(const archive::ArchiveSet& set,
                                                        const std::string& vpath);

/// Resolve a data folder's load order without a list file (as
/// `bethconv loadorder --data` does) and summarize placements, e.g. that
/// `_ResourcePack.esl` lands in the 0xFE space.
[[nodiscard]] LoadOrderFacts probe_load_order(const std::filesystem::path& data_dir);

/// Read one NIF from `set`, convert it and summarize both sides. Nullopt if the
/// path is absent (a skip); unparseable files give `flavor` "parse-error".
/// Write options are spelled out because `glb_hash` depends on them.
[[nodiscard]] std::optional<MeshFacts> probe_mesh(const archive::ArchiveSet& set,
                                                  const std::string& vpath);

/// Run every asset pass over `set` into `out` and summarize the pack.
///
/// `out` must be empty: PackWriter reuses existing assets, which would turn a
/// second run into all `deduped`. `records.fb` is not written; merge and
/// snapshot are pinned separately.
///
/// Options are spelled out because asset names depend on them. The converter
/// string is fixed instead of `BETHCONV_VERSION`, since a version bump renames
/// every asset.
[[nodiscard]] std::optional<ConvertFacts> probe_convert(const std::filesystem::path& data_dir,
                                                        const archive::ArchiveSet& set,
                                                        const std::filesystem::path& out,
                                                        const std::string& filter,
                                                        std::size_t limit);

/// Materialize `pack` into `out` and summarize. Uses the pack `probe_convert`
/// just built rather than converting again.
[[nodiscard]] std::optional<ViewFacts> probe_view(const std::filesystem::path& pack,
                                                  const std::filesystem::path& out,
                                                  const std::string& filter, std::size_t limit);

/// Build a data folder's load order, merge it and summarize, including the
/// second pass.
///
/// `sources` supplies `strings/` bytes; it is the set the archive pass
/// mounted. Mount order matters: on VR, `Skyrim - Patch.bsa` and
/// `Skyrim - Interface.bsa` provide 105 `strings/` paths with different
/// contents, deciding whether 25 `Update.esm` GMST names resolve. The
/// expectations list archives in folder order, so Patch mounts later and wins,
/// as in the game's `sResourceArchiveList2`.
///
/// `pinned` lists global FormIDs (hex text) to record. Unknown ones come back
/// with `present` false instead of being dropped.
///
/// `scratch` receives `records.fb` and `world.fb` for the snapshot and world
/// pins; empty skips them.
[[nodiscard]] std::optional<MergeFacts> probe_merge(const std::filesystem::path& data_dir,
                                                    const archive::ArchiveSet& sources,
                                                    const std::vector<std::string>& pinned,
                                                    const std::filesystem::path& scratch = {});

/// Run every field definition over every record of the named plugins and
/// summarize.
///
/// `plugins` is the same list the plugin pin walks; if any is missing, skip
/// this pin rather than record smaller totals. `sources` must include the
/// string archives, otherwise `unresolved_strings` would be 0 because nothing
/// was asked; `string_tables` guards against that.
[[nodiscard]] std::optional<FormsFacts> probe_forms(const std::filesystem::path& data_dir,
                                                    const archive::ArchiveSet& sources,
                                                    const std::vector<std::string>& plugins);

/// Decode every script in `sources`.
[[nodiscard]] ScriptFacts probe_scripts(const archive::ArchiveSet& sources);
[[nodiscard]] AnimationFacts probe_animations(const archive::ArchiveSet& sources);

} // namespace bethconv::corpus
