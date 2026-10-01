// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/pack_writer.hpp"

#include "bethconv/io/span_stream.hpp"

#include "bethconv/io/json_text.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <system_error>
#include <utility>

namespace bethconv::pack {
namespace {

using nlohmann::ordered_json;
using bethconv::io::json_text;

[[nodiscard]] std::unexpected<io::ParseError> write_error(const std::filesystem::path& path,
                                                          std::string detail) {
    return std::unexpected(io::ParseError{.origin = path.string(),
                                          .offset = 0,
                                          .kind = io::ErrorKind::corrupt,
                                          .detail = std::move(detail)});
}

[[nodiscard]] std::span<const std::byte> as_bytes(const std::string& text) {
    return std::as_bytes(std::span<const char>(text));
}

} // namespace

PackWriter::~PackWriter() = default;
PackWriter::PackWriter(PackWriter&&) noexcept = default;
PackWriter& PackWriter::operator=(PackWriter&&) noexcept = default;

io::ParseResult<PackWriter> PackWriter::create(const std::filesystem::path& root,
                                               PackOptions options) {
    auto store = AssetStore::open(root, options.layout);
    if (!store) {
        return std::unexpected(store.error());
    }
    PackWriter writer;
    writer.root_ = root;
    writer.options_ = std::move(options);
    writer.store_ = std::move(*store);
    return writer;
}

std::filesystem::path PackWriter::records_path() const { return root_ / "records.fb"; }
std::filesystem::path PackWriter::world_path() const { return root_ / "world.fb"; }

std::string PackWriter::settings_for(AssetKind kind) const {
    switch (kind) {
    case AssetKind::mesh: return options_.mesh_settings;
    case AssetKind::texture: return options_.texture_settings;
    case AssetKind::script: return options_.script_settings;
    case AssetKind::lod: return options_.lod_settings;
    }
    return {};
}

AssetSlot PackWriter::reserve(std::string_view vpath, AssetKind kind,
                              std::span<const std::byte> source, std::string_view source_name) {
    AssetSlot slot;
    slot.kind = kind;
    slot.hash = content_hash(source, options_.converter, settings_for(kind));
    slot.already_present = store_.contains(slot.hash);
    slot.vpath = std::string(vpath);
    slot.source = std::string(source_name);
    slot.source_bytes = source.size();

    ++stats_.inputs;
    stats_.source_bytes += source.size();
    return slot;
}

void PackWriter::record(const AssetSlot& slot) {
    index_.push_back(IndexEntry{.vpath = slot.vpath,
                                .hex = slot.hash.hex(),
                                .kind = slot.kind,
                                .source = slot.source});
}

io::ParseResult<void> PackWriter::store(const AssetSlot& slot,
                                        std::span<const std::byte> converted) {
    if (auto put = store_.put(slot.hash, slot.kind, converted); !put) {
        return std::unexpected(put.error());
    }
    record(slot);

    ++stats_.converted;
    stats_.asset_bytes += converted.size();
    switch (slot.kind) {
    case AssetKind::mesh: ++stats_.meshes; break;
    case AssetKind::texture: ++stats_.textures; break;
    case AssetKind::script: ++stats_.scripts; break;
    case AssetKind::lod: ++stats_.lod; break;
    }
    return {};
}

void PackWriter::reuse(const AssetSlot& slot) {
    record(slot);
    ++stats_.deduped;
    stats_.dedupe_saved_bytes += slot.source_bytes;
    switch (slot.kind) {
    case AssetKind::mesh: ++stats_.meshes; break;
    case AssetKind::texture: ++stats_.textures; break;
    case AssetKind::script: ++stats_.scripts; break;
    case AssetKind::lod: ++stats_.lod; break;
    }
}

void PackWriter::fail(PackFailure failure) {
    ++stats_.failed;
    failures_.push_back(std::move(failure));
}

void PackWriter::warn(PackWarning warning) {
    ++stats_.warnings;
    warnings_.push_back(std::move(warning));
}

void PackWriter::defer(std::string_view extension) {
    ++stats_.deferred;
    ++deferred_[std::string(extension)].files;
}

io::ParseResult<PackStats> PackWriter::finish(const PackManifest& manifest) {
    // Sorted so identical runs give byte-identical packs.
    std::ranges::sort(index_, [](const IndexEntry& a, const IndexEntry& b) {
        return a.vpath != b.vpath ? a.vpath < b.vpath : a.hex < b.hex;
    });
    std::ranges::sort(failures_, [](const PackFailure& a, const PackFailure& b) {
        return a.vpath != b.vpath ? a.vpath < b.vpath : a.stage < b.stage;
    });
    std::ranges::sort(warnings_, [](const PackWarning& a, const PackWarning& b) {
        return a.vpath != b.vpath ? a.vpath < b.vpath : a.detail < b.detail;
    });

    std::unordered_set<std::string> referenced;
    for (const auto& entry : index_) {
        referenced.insert(entry.hex);
    }
    stats_.distinct_assets = referenced.size();
    stats_.index_entries = index_.size();
    stats_.deferred_kinds = deferred_.size();

    // ---- vpath.idx ----------------------------------------------------
    std::string index_text = index_header();
    for (const auto& entry : index_) {
        index_text += format_index_line(entry.vpath, entry.hex, entry.kind, entry.source);
    }
    const auto index_path = root_ / "vpath.idx";
    std::string error;
    if (!io::write_file(index_path, as_bytes(index_text), error)) {
        return write_error(index_path, std::move(error));
    }
    stats_.index_bytes = index_text.size();

    // ---- the store ----------------------------------------------------
    // Before the report: pruning changes the counts it lists.
    stats_.orphaned_assets = 0;
    auto stored = store_.finish(referenced, options_.prune_orphans);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    stats_.orphaned_assets = stored->orphaned;
    stats_.pruned_assets += stored->pruned;
    stats_.store_bytes = stored->bytes;

    // ---- report.json --------------------------------------------------
    ordered_json report;
    report["pack_format_version"] = k_pack_format_version;
    report["totals"] = ordered_json{
        {"inputs", stats_.inputs},         {"converted", stats_.converted},
        {"deduped", stats_.deduped},       {"failed", stats_.failed},
        {"deferred", stats_.deferred},     {"warnings", stats_.warnings},
        {"distinct_assets", stats_.distinct_assets},
        {"index_entries", stats_.index_entries},
        {"orphaned_assets", stats_.orphaned_assets},
        {"pruned_assets", stats_.pruned_assets},
    };
    report["bytes"] = ordered_json{
        {"source", stats_.source_bytes},
        {"assets", stats_.asset_bytes},
        {"store", stats_.store_bytes},
        {"dedupe_saved", stats_.dedupe_saved_bytes},
    };
    report["assets"] = ordered_json{
        {"meshes", stats_.meshes}, {"textures", stats_.textures}, {"scripts", stats_.scripts},
        {"lod", stats_.lod}};

    // Uncapped: large broken load orders are where the full list matters.
    auto failures = ordered_json::array();
    for (const auto& failure : failures_) {
        failures.push_back(ordered_json{{"vpath", json_text(failure.vpath)},
                                        {"stage", failure.stage},
                                        {"kind", failure.kind},
                                        {"detail", json_text(failure.detail)}});
    }
    report["failures"] = std::move(failures);

    auto warnings = ordered_json::array();
    for (const auto& warning : warnings_) {
        warnings.push_back(
            ordered_json{{"vpath", json_text(warning.vpath)}, {"detail", json_text(warning.detail)}});
    }
    report["warnings"] = std::move(warnings);

    auto deferred = ordered_json::object();
    for (const auto& [extension, counts] : deferred_) {
        deferred[json_text(extension)] = counts.files;
    }
    report["deferred"] = std::move(deferred);

    const std::string report_text = report.dump(2) + "\n";
    const auto report_path = root_ / "report.json";
    if (!io::write_file(report_path, as_bytes(report_text), error)) {
        return write_error(report_path, std::move(error));
    }
    stats_.report_bytes = report_text.size();

    // ---- manifest.json ------------------------------------------------
    // No timestamp; it would break determinism.
    ordered_json doc;
    doc["pack_format_version"] = k_pack_format_version;
    doc["converter"] = manifest.converter;
    doc["language"] = json_text(manifest.language);
    if (manifest.input) {
        const auto& in = *manifest.input;
        ordered_json input{{"kind", json_text(in.kind)},
                           {"edition", json_text(in.edition)},
                           {"data", json_text(in.data)},
                           {"plugin_list", json_text(in.plugin_list)}};
        if (in.kind == "mo2") {
            input["mo2_instance"] = json_text(in.mo2_instance);
            input["mo2_profile"] = json_text(in.mo2_profile);
            input["mods"] = in.mods;
        }
        doc["input"] = std::move(input);
    }
    if (manifest.textures) {
        doc["textures"] = ordered_json{{"max_size", manifest.textures->max_size},
                                       {"complete_mip_chains", manifest.textures->complete_mip_chains}};
    }

    auto order = ordered_json::array();
    for (const auto& plugin : manifest.load_order) {
        order.push_back(json_text(plugin));
    }
    doc["load_order"] = std::move(order);

    auto sources = ordered_json::array();
    for (const auto& source : manifest.sources) {
        ordered_json entry{
            {"name", json_text(source.name)}, {"kind", source.kind}, {"bytes", source.bytes}};
        if (source.hash) {
            entry["hash"] = source.hash->hex();
        }
        sources.push_back(std::move(entry));
    }
    doc["source_hashes"] = std::move(sources);

    if (manifest.records) {
        doc["records"] = ordered_json{{"file", "records.fb"},
                                      {"forms", manifest.records->forms},
                                      {"bytes", manifest.records->file_bytes},
                                      {"hash", manifest.records->hash.hex()}};
    }
    if (manifest.world) {
        doc["world"] = ordered_json{{"file", "world.fb"},
                                    {"cells", manifest.world->cells},
                                    {"refs", manifest.world->refs},
                                    {"bases", manifest.world->bases},
                                    {"bytes", manifest.world->file_bytes},
                                    {"hash", manifest.world->hash.hex()}};
    }

    doc["assets"] = ordered_json{{"distinct", stats_.distinct_assets},
                                 {"index_entries", stats_.index_entries},
                                 {"meshes", stats_.meshes},
                                 {"textures", stats_.textures},
                                 {"scripts", stats_.scripts},
                                 {"lod", stats_.lod},
                                 {"bytes", stats_.asset_bytes},
                                 {"dedupe_saved_bytes", stats_.dedupe_saved_bytes}};

    // How the engine finds asset bytes: an index plus one blob, or loose files.
    if (store_.layout() == StoreLayout::blob) {
        doc["store"] = ordered_json{{"layout", "blob"},
                                    {"index", "assets.idx"},
                                    {"blob", blob_file_name(store_.generation())},
                                    {"bytes", stats_.store_bytes}};
    } else {
        doc["store"] = ordered_json{{"layout", "loose"}, {"bytes", stats_.store_bytes}};
    }

    auto not_converted = ordered_json::object();
    for (const auto& [extension, counts] : deferred_) {
        not_converted[json_text(extension)] = counts.files;
    }
    doc["deferred"] = std::move(not_converted);
    doc["report"] = ordered_json{{"file", "report.json"},
                                 {"failed", stats_.failed},
                                 {"warnings", stats_.warnings}};

    const std::string manifest_text = doc.dump(2) + "\n";
    const auto manifest_path = root_ / "manifest.json";
    if (!io::write_file(manifest_path, as_bytes(manifest_text), error)) {
        return write_error(manifest_path, std::move(error));
    }
    stats_.manifest_bytes = manifest_text.size();

    return stats_;
}

} // namespace bethconv::pack
