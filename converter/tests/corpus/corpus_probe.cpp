// SPDX-License-Identifier: GPL-3.0-or-later
#include "corpus_probe.hpp"

#include "bethconv/io/mapped_file.hpp"
#include "bethconv/io/span_reader.hpp"
#include "bethconv/pack/script_asset.hpp"
#include "bethconv/pack/snapshot.hpp"
#include "bethconv/pack/world.hpp"
#include "bethconv/record/form_census.hpp"
#include "bethconv/record/strings.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace bethconv::corpus {
namespace {

/// Histogram as "TYPE count\n" lines in numeric FourCC order (stable across
/// platforms).
std::string render_histogram(const record::Histogram& histogram) {
    std::string out;
    for (const auto& [type, stats] : histogram.types()) {
        out += io::FourCC{type}.to_string();
        out += ' ';
        out += std::to_string(stats.count);
        out += '\n';
    }
    return out;
}

/// Merged per-type totals, rendered like render_histogram: one hash instead of
/// one expectation per type.
std::string render_type_counts(const std::map<std::uint32_t, std::uint64_t>& counts) {
    std::string out;
    for (const auto& [type, count] : counts) {
        out += io::FourCC{type}.to_string();
        out += ' ';
        out += std::to_string(count);
        out += '\n';
    }
    return out;
}

/// Field census as "TYPE seen parsed failed\n" per type, in FourCC order.
/// Counts records a definition ran on, not records a walk saw.
std::string render_form_types(const std::map<std::uint32_t, record::FormTypeStats>& types) {
    std::string out;
    for (const auto& [type, stats] : types) {
        out += io::FourCC{type}.to_string();
        out += ' ';
        out += std::to_string(stats.seen);
        out += ' ';
        out += std::to_string(stats.parsed);
        out += ' ';
        out += std::to_string(stats.failed);
        out += '\n';
    }
    return out;
}

/// Unclaimed fields as "TYPE FIELD count\n", so they are pinned by content, not
/// just by total.
std::string render_unhandled(const std::map<std::uint32_t, record::FormTypeStats>& types) {
    std::string out;
    for (const auto& [type, stats] : types) {
        for (const auto& [field, count] : stats.unhandled) {
            out += io::FourCC{type}.to_string();
            out += ' ';
            out += io::FourCC{field}.to_string();
            out += ' ';
            out += std::to_string(count);
            out += '\n';
        }
    }
    return out;
}

/// "0x00027D1C" -> FormId, or nullopt if it is not a FormID. Not zero: a typo
/// must not turn into "pin the null form".
std::optional<record::FormId> parse_form(const std::string& text) {
    std::string_view digits(text);
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
        digits.remove_prefix(2);
    }
    if (digits.empty() || digits.size() > 8) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const auto* const end = digits.data() + digits.size();
    const auto result = std::from_chars(digits.data(), end, value, 16);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return record::FormId{value};
}

/// Pass two, summarized: counts every forwarded record (half of the integrity
/// check) and records the pinned forms.
class PinSink final : public record::MergedRecordSink {
public:
    PinSink(std::unordered_map<std::uint32_t, std::size_t> wanted,
            std::vector<MergedFormFacts>& out)
        : wanted_(std::move(wanted)), out_(out) {}

    void on_record(const record::MergedRecord& merged, const record::RecordContext& ctx,
                   io::SpanReader& data, const record::FormContext& form_ctx) override {
        ++forwarded_;
        const auto slot = wanted_.find(merged.form.value);
        if (slot == wanted_.end()) {
            return;
        }
        auto& facts = out_[slot->second];
        facts.present = true;

        // Copy the cursor so hashing does not consume what the parsers need.
        io::SpanReader payload = data;
        facts.payload_bytes = payload.size();
        if (const auto bytes = payload.bytes(payload.size())) {
            facts.payload_hash = fnv1a(*bytes);
        }

        describe(merged.type, ctx, data, form_ctx, facts);
    }

    [[nodiscard]] std::uint64_t forwarded() const noexcept { return forwarded_; }

private:
    /// Hash identifying strings for the six types handled here (CELL, WRLD,
    /// DOOR, LIGH, STAT, REFR). Other types keep zero hashes but still pin
    /// their header and bytes.
    static void describe(record::FourCC type, const record::RecordContext& ctx,
                         io::SpanReader& data, const record::FormContext& form_ctx,
                         MergedFormFacts& facts) {
        using namespace bethconv::record;

        const auto set = [&facts](const std::string& editor_id, const std::string& name,
                                  bool from_table) {
            facts.editor_id_hash = editor_id.empty() ? 0 : fnv1a(editor_id);
            facts.name_hash = name.empty() ? 0 : fnv1a(name);
            facts.name_from_table = from_table;
        };

        if (type == FourCC{"CELL"}) {
            if (const auto cell = parse_cell(data, form_ctx)) {
                set(cell->editor_id, cell->name.to_string(), cell->name.from_table());
            }
        } else if (type == FourCC{"WRLD"}) {
            if (const auto world = parse_worldspace(data, form_ctx)) {
                set(world->editor_id, world->name.to_string(), world->name.from_table());
            }
        } else if (type == FourCC{"DOOR"}) {
            if (const auto door = parse_door(data, form_ctx)) {
                set(door->editor_id, door->name.to_string(), door->name.from_table());
            }
        } else if (type == FourCC{"LIGH"}) {
            if (const auto light = parse_light(data, form_ctx)) {
                set(light->editor_id, light->name.to_string(), light->name.from_table());
            }
        } else if (type == FourCC{"STAT"}) {
            // STAT has no FULL; its model path identifies it (and is what the
            // mesh pass resolves).
            if (const auto stat = parse_static(data, form_ctx)) {
                set(stat->editor_id, stat->model.path, false);
            }
        } else if (type == FourCC{"REFR"}) {
            // No name: pin the placement (base object and position), with fixed
            // decimals so the hash does not depend on printf.
            if (const auto refr = parse_reference(ctx.header, data, form_ctx)) {
                char buffer[96]{};
                std::snprintf(buffer, sizeof(buffer), "%s %.4f %.4f %.4f",
                              refr->base.to_string().c_str(),
                              static_cast<double>(refr->position.x),
                              static_cast<double>(refr->position.y),
                              static_cast<double>(refr->position.z));
                set(refr->editor_id, buffer, false);
            }
        }
    }

    std::unordered_map<std::uint32_t, std::size_t> wanted_;
    std::vector<MergedFormFacts>& out_;
    std::uint64_t forwarded_ = 0;
};

/// One form per merge class, in index order, so repeated runs name the same
/// forms.
std::vector<std::string> suggest_forms(const record::MergedWorld& world) {
    using Predicate = bool (*)(const record::MergedRecord&);
    const std::pair<const char*, Predicate> classes[] = {
        {"plain", [](const record::MergedRecord& r) {
             return r.overrides == 0 && !r.deleted && !r.injected && r.parent.is_null();
         }},
        {"overridden", [](const record::MergedRecord& r) { return r.overrides > 0; }},
        {"deleted", [](const record::MergedRecord& r) { return r.deleted; }},
        {"injected", [](const record::MergedRecord& r) { return r.injected; }},
        {"parented", [](const record::MergedRecord& r) { return !r.parent.is_null(); }},
        {"light-space", [](const record::MergedRecord& r) { return r.form.is_light(); }},
        {"CELL", [](const record::MergedRecord& r) { return r.type == io::FourCC{"CELL"}; }},
        {"WRLD", [](const record::MergedRecord& r) { return r.type == io::FourCC{"WRLD"}; }},
        {"REFR", [](const record::MergedRecord& r) { return r.type == io::FourCC{"REFR"}; }},
        {"DOOR", [](const record::MergedRecord& r) { return r.type == io::FourCC{"DOOR"}; }},
        {"LIGH", [](const record::MergedRecord& r) { return r.type == io::FourCC{"LIGH"}; }},
        {"STAT", [](const record::MergedRecord& r) { return r.type == io::FourCC{"STAT"}; }},
    };

    std::vector<std::string> found;
    for (const auto& [label, matches] : classes) {
        const auto hit = std::ranges::find_if(
            world.records(), [&](const record::MergedRecord& r) { return matches(r); });
        if (hit == world.records().end()) {
            continue;
        }
        found.push_back(std::string(label) + " " + hit->type.to_string() + " " +
                        hit->form.to_string());
    }
    return found;
}

} // namespace

std::optional<PluginFacts> probe_plugin(const std::filesystem::path& path) {
    auto plugin = record::Plugin::open(path);
    if (!plugin) {
        return std::nullopt;
    }

    record::Histogram histogram;
    const auto stats = plugin->scan(histogram);
    const auto& header = plugin->header();

    return PluginFacts{
        .file = path.filename().string(),
        // Size of the mapped file, so it describes the same bytes as the counts.
        .file_bytes = plugin->size(),
        .header_version = header.version,
        .hedr_record_count = header.record_count,
        .records = stats.records,
        .groups = stats.groups,
        .compressed_records = stats.compressed_records,
        .errors = stats.errors,
        .record_types = histogram.types().size(),
        .masters = header.masters.size(),
        .is_master = header.is_master(),
        .is_light = header.is_light(),
        .is_localized = header.is_localized(),
        .type_histogram_hash = fnv1a(render_histogram(histogram)),
        // Basis points, truncated: floats in the expectations would fail on the
        // last digit.
        .field_coverage_bp =
            static_cast<std::uint64_t>(histogram.field_coverage() * 10000.0),
    };
}

ArchiveScan probe_archives(const std::filesystem::path& data_dir,
                           const std::vector<std::string>& archives,
                           archive::ArchiveSet& set) {
    ArchiveScan scan;

    for (const auto& name : archives) {
        const auto path = data_dir / name;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            scan.missing.push_back(name);
            continue;
        }
        // Equal priority: mount order alone decides, so shadow counts describe
        // the install.
        const auto mounted = set.mount_archive(path, 0);
        if (!mounted) {
            scan.archives.push_back(ArchiveFacts{
                .file = name, .kind = "unmountable", .version = 0, .files = 0});
            continue;
        }
        const auto& info = set.sources().back();
        scan.archives.push_back(
            ArchiveFacts{.file = name,
                         .kind = std::string(archive::to_string(info.kind)),
                         .version = info.version,
                         .files = info.file_count});
    }

    scan.unique_paths = set.unique_paths();
    scan.shadowed = set.conflicts().size();
    return scan;
}

LoadOrderFacts probe_load_order(const std::filesystem::path& data_dir) {
    LoadOrderFacts facts;
    const auto order = record::LoadOrder::from_directory(data_dir);
    if (!order) {
        return facts;
    }
    for (const auto& entry : order->entries()) {
        char index[16]{};
        if (entry.is_light) {
            std::snprintf(index, sizeof(index), "FE:%03X", entry.index);
        } else {
            std::snprintf(index, sizeof(index), "%02X", entry.index);
        }
        std::string flags;
        flags += entry.is_master ? 'M' : '-';
        flags += entry.is_light ? 'L' : '-';
        flags += entry.is_localized ? 'S' : '-';
        facts.plugins.push_back(PlacedPlugin{.name = entry.name,
                                             .index = index,
                                             .flags = std::move(flags),
                                             .masters = entry.masters.size()});
    }
    facts.normal_used = order->normal_count();
    facts.light_used = order->light_count();
    facts.problems = order->problems().size();
    return facts;
}

std::optional<TextureFacts> probe_texture(const archive::ArchiveSet& set,
                                          const std::string& vpath) {
    const auto bytes = set.read(vpath);
    if (!bytes) {
        return std::nullopt;
    }

    TextureFacts facts;
    facts.vpath = vpath;
    facts.output_bytes = bytes->size();
    facts.output_hash = fnv1a(*bytes);

    const auto info = texture::parse_dds(*bytes, vpath);
    if (!info) {
        // Record parse failures as a fact instead of skipping the file.
        facts.format = std::string(io::to_string(info.error().kind));
        facts.kind = "unparsed";
        facts.outcome = "parse-error";
        return facts;
    }

    facts.format = info->layout.name;
    facts.kind = std::string(texture::to_string(info->kind));
    facts.width = info->width;
    facts.height = info->height;
    facts.faces = info->faces;
    facts.stored_levels = info->stored_levels;
    facts.full_chain_levels = info->full_chain_levels;
    facts.declared_bytes = info->declared_bytes;

    const auto fix = texture::complete_mip_tail(*bytes, *info, vpath);
    if (!fix) {
        facts.outcome = "fix-error";
        return facts;
    }
    facts.outcome = std::string(texture::to_string(fix->outcome));
    if (fix->outcome == texture::TailOutcome::completed) {
        facts.output_bytes = fix->data.size();
        facts.output_hash = fnv1a(std::span<const std::byte>(fix->data));
    }
    return facts;
}

std::optional<MeshFacts> probe_mesh(const archive::ArchiveSet& set, const std::string& vpath) {
    const auto bytes = set.read(vpath);
    if (!bytes) {
        return std::nullopt;
    }

    MeshFacts facts;
    facts.vpath = vpath;

    const auto model = mesh::read_nif(*bytes, vpath);
    if (!model) {
        // Same as for textures: an unreadable mesh is recorded, not skipped.
        facts.flavor = "parse-error";
        facts.nif_version = std::string(io::to_string(model.error().kind));
        return facts;
    }

    facts.flavor = std::string(mesh::to_string(mesh::flavor_of(model->nif_stream)));
    facts.nif_version = model->nif_version;
    facts.nif_stream = model->nif_stream;
    facts.nodes = model->nodes.size();
    facts.roots = model->roots.size();
    facts.primitives = model->primitives.size();
    facts.materials = model->materials.size();
    facts.skins = model->skins.size();
    facts.collision = model->collision.size();
    facts.warnings = model->warnings.size();
    for (const auto& skin : model->skins) {
        facts.joints += skin.joints.size();
    }
    for (const auto& prim : model->primitives) {
        facts.vertices += prim.positions.size();
        facts.triangles += prim.indices.size() / 3;
        facts.with_normals += prim.normals.empty() ? 0U : 1U;
        facts.with_tangents += prim.tangents.empty() ? 0U : 1U;
        facts.with_uvs += prim.uvs.empty() ? 0U : 1U;
        facts.with_colors += prim.colors.empty() ? 0U : 1U;
        facts.with_joints += prim.joints.empty() ? 0U : 1U;
    }

    // The CLI defaults, spelled out: glb_hash depends on them.
    const mesh::WriteOptions options{
        .convert_to_y_up = true,
        .unit_scale = 0.0142875f,
        .texture_refs = mesh::TextureRefs::source_paths,
        .write_extras = true,
    };
    const auto glb = mesh::write_glb(*model, options);
    if (!glb) {
        facts.glb_bytes = 0;
        facts.glb_hash = 0;
        return facts;
    }
    facts.glb_bytes = glb->size();
    facts.glb_hash = fnv1a(std::span<const std::byte>(*glb));

    // The convert pin's two GLB checks, on chosen files: a FaceGen head with a
    // control byte in its normal-map slot, and greybeardstatic.nif with its NaN
    // transform.
    const auto check = inspect_glb(*glb);
    facts.glb_control_bytes = check.control_bytes;
    facts.glb_json_parses = check.json_parses;
    facts.images = check.images;
    facts.escaped_uris = check.escaped_uris;
    return facts;
}

ReadCheck check_reads(const archive::ArchiveSet& set,
                      const std::vector<std::string>& vpaths) {
    ReadCheck check;
    for (const auto& vpath : vpaths) {
        if (!set.resolve(vpath)) {
            check.unresolved.push_back(vpath);
        } else if (set.read(vpath)) {
            check.succeeded.push_back(vpath);
        } else {
            check.failed.push_back(vpath);
        }
    }
    return check;
}


/// glTF 2.0 §3.2/§3.3: `glTF` magic, version 2, first chunk JSON. Written here
/// independently of pack_view.cpp so the reader under test is not checking
/// itself.
GlbCheck inspect_glb(std::span<const std::byte> glb) {
    GlbCheck check;

    io::SpanReader reader(glb, "glb");
    const auto magic = reader.tag();
    const auto version = reader.get<std::uint32_t>();
    const auto total = reader.get<std::uint32_t>();
    const auto chunk_length = reader.get<std::uint32_t>();
    const auto chunk_type = reader.tag();
    if (!magic || !version || !total || !chunk_length || !chunk_type) {
        return check;
    }
    if (*magic != io::FourCC{"glTF"} || *version != 2U || *chunk_type != io::FourCC{"JSON"}) {
        return check;
    }
    const auto text = reader.chars(*chunk_length);
    if (!text) {
        return check;
    }
    check.container_ok = true;

    // Check one: Blender rejects the file for a single one ("Invalid control
    // character"). Counted to show how many.
    for (const char c : *text) {
        if (static_cast<unsigned char>(c) < 0x20) {
            ++check.control_bytes;
        }
    }

    // Check two: strict parsing. nlohmann rejects out-of-range numbers as
    // "number overflow"; Python's json accepts them, which is how an earlier
    // "all 22,573 parse" claim was wrong.
    const auto doc = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        return check;
    }
    check.json_parses = true;

    const auto images = doc.find("images");
    if (images == doc.end() || !images->is_array()) {
        return check;
    }
    for (const auto& image : *images) {
        if (!image.is_object()) {
            continue;
        }
        const auto uri = image.find("uri");
        if (uri == image.end() || !uri->is_string()) {
            continue;
        }
        ++check.images;
        if (uri->get<std::string>().find('%') != std::string::npos) {
            ++check.escaped_uris;
        }
    }
    return check;
}

namespace {

/// FNV-1a over a file, or 0 if missing (e.g. `finish()` never wrote
/// report.json).
std::uint64_t hash_whole_file(const std::filesystem::path& path) {
    const auto mapped = io::MappedFile::open(path);
    if (!mapped) {
        return 0;
    }
    return fnv1a(mapped->bytes());
}

} // namespace

std::optional<ConvertFacts> probe_convert(const std::filesystem::path& data_dir,
                                          const archive::ArchiveSet& set,
                                          const std::filesystem::path& out,
                                          const std::string& filter, std::size_t limit) {
    // Load order from the folder, not a plugins.txt: vanilla Steam installs
    // have none, and the fallback is deterministic.
    const auto order = record::LoadOrder::from_directory(data_dir);
    if (!order) {
        return std::nullopt;
    }

    pack::ConvertOptions options;
    options.out = out;
    // Fixed name instead of BETHCONV_VERSION: a version bump renames every
    // asset and would turn every release red.
    options.converter = "bethconv-corpus";
    options.language = std::string(record::k_default_language);
    // Merge and snapshot are pinned separately and are the expensive part.
    options.write_records = false;
    options.convert_meshes = true;
    options.convert_textures = true;
    options.convert_scripts = true;
    options.convert_lod = true;
    options.fix_mip_tail = true;
    options.mesh_read.read_collision = true;
    options.mesh_read.read_skinning = true;
    options.mesh_write.convert_to_y_up = true;
    options.mesh_write.unit_scale = 0.0142875f;
    // Virtual-path texture references, as in a pack. Part of the mesh
    // fingerprint, so part of every name in `index_hash`.
    options.mesh_write.texture_refs = mesh::TextureRefs::pack_vpaths;
    options.mesh_write.write_extras = true;
    options.filter = filter;
    options.limit = limit;
    options.hash_archives = false;
    options.prune_orphans = false;

    const auto result = pack::convert(set, *order, options);
    if (!result) {
        return std::nullopt;
    }

    ConvertFacts facts;
    facts.filter = filter;
    facts.limit = limit;
    facts.unique_paths = result->unique_paths;
    facts.considered = result->considered;

    const auto& stats = result->pack;
    facts.inputs = stats.inputs;
    facts.converted = stats.converted;
    facts.deduped = stats.deduped;
    facts.deferred = stats.deferred;
    facts.deferred_kinds = stats.deferred_kinds;
    facts.failed = stats.failed;
    facts.warnings = stats.warnings;
    facts.meshes = stats.meshes;
    facts.textures = stats.textures;
    facts.scripts = stats.scripts;
    facts.lod = stats.lod;
    facts.distinct_assets = stats.distinct_assets;
    facts.index_entries = stats.index_entries;
    facts.orphaned_assets = stats.orphaned_assets;
    facts.asset_bytes = stats.asset_bytes;
    facts.source_bytes = stats.source_bytes;
    facts.dedupe_saved_bytes = stats.dedupe_saved_bytes;

    facts.manifest_hash = hash_whole_file(out / "manifest.json");
    facts.index_hash = hash_whole_file(out / "vpath.idx");
    facts.report_hash = hash_whole_file(out / "report.json");

    // Check every GLB the run wrote (control bytes, strict JSON).
    std::error_code ec;
    const std::filesystem::path assets = out / "assets";
    if (std::filesystem::is_directory(assets, ec)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(assets, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".glb") {
                continue;
            }
            const auto mapped = io::MappedFile::open(entry.path());
            if (!mapped) {
                ++facts.glb_bad_container;
                continue;
            }
            ++facts.glb_checked;
            const auto check = inspect_glb(mapped->bytes());
            if (!check.container_ok) {
                ++facts.glb_bad_container;
            }
            if (!check.json_parses) {
                ++facts.glb_unparseable;
            }
            facts.glb_control_bytes += check.control_bytes;
            facts.glb_escaped_uris += check.escaped_uris;
        }
    }
    return facts;
}

std::optional<ViewFacts> probe_view(const std::filesystem::path& pack,
                                    const std::filesystem::path& out, const std::string& filter,
                                    std::size_t limit) {
    pack::ViewOptions options;
    options.out = out;
    // Links, as the CLI default. If scratch and pack were on different
    // filesystems the view would copy, visible as `copied` instead of `linked`.
    options.copy_assets = false;
    options.filter = filter;
    options.limit = limit;

    const auto result = pack::materialize_view(pack, options);
    if (!result) {
        return std::nullopt;
    }

    const auto& stats = result->stats;
    ViewFacts facts;
    facts.filter = filter;
    facts.limit = limit;
    facts.index_entries = stats.index_entries;
    facts.considered = stats.considered;
    facts.meshes = stats.meshes;
    facts.textures = stats.textures;
    facts.scripts = stats.scripts;
    facts.lod = stats.lod;
    facts.linked = stats.linked;
    facts.copied = stats.copied;
    facts.written = stats.written;
    facts.mesh_links = stats.mesh_links;
    facts.pulled_in = stats.pulled_in;
    facts.image_refs = stats.image_refs;
    facts.image_refs_resolved = stats.image_refs_resolved;
    facts.image_refs_dangling = stats.image_refs_dangling;
    facts.failed = stats.failed;
    facts.bytes = stats.bytes;

    // Check what the view wrote: it re-serializes each mesh's JSON and is the
    // last writer before a consumer.
    std::error_code ec;
    if (std::filesystem::is_directory(out, ec)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(out, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".glb") {
                continue;
            }
            const auto mapped = io::MappedFile::open(entry.path());
            if (!mapped) {
                ++facts.glb_bad_container;
                continue;
            }
            ++facts.glb_checked;
            const auto check = inspect_glb(mapped->bytes());
            if (!check.container_ok) {
                ++facts.glb_bad_container;
            }
            if (!check.json_parses) {
                ++facts.glb_unparseable;
            }
            facts.glb_control_bytes += check.control_bytes;
        }
    }
    return facts;
}

std::optional<MergeFacts> probe_merge(const std::filesystem::path& data_dir,
                                      const archive::ArchiveSet& sources,
                                      const std::vector<std::string>& pinned,
                                      const std::filesystem::path& scratch) {
    // From the folder, as in probe_load_order: deterministic for vanilla.
    const auto order = record::LoadOrder::from_directory(data_dir);
    if (!order) {
        return std::nullopt;
    }

    record::MergeOptions options;
    // Same wiring as `bethconv merge`.
    options.strings = [&sources](std::string_view vpath)
        -> std::optional<std::vector<std::byte>> {
        auto bytes = sources.read(vpath);
        if (!bytes) {
            return std::nullopt;
        }
        return std::move(*bytes);
    };

    // `order` outlives the world, as MergedWorld::build requires.
    const auto world = record::MergedWorld::build(*order, options);
    const auto& stats = world.stats();

    MergeFacts facts{
        .plugins = stats.plugins,
        .visited = stats.visited,
        .forms = stats.forms,
        .collapsed = stats.collapsed,
        .deleted = stats.deleted,
        .injected = stats.injected,
        .unresolved = stats.unresolved,
        .unparented = stats.unparented,
        .errors = stats.errors,
        .unreadable = stats.unreadable,
        .string_tables = stats.string_tables,
        .problems = world.problems().size(),
        .type_counts_hash = fnv1a(render_type_counts(world.type_counts())),
        .forms_pinned = {},
        .snapshot = {},
        .world = {},
        .suggestions = suggest_forms(world),
    };

    // Record index facts for the pinned forms before the second pass, so a
    // form that is indexed but never forwarded shows up via `present`.
    std::unordered_map<std::uint32_t, std::size_t> wanted;
    for (const auto& text : pinned) {
        MergedFormFacts entry;
        entry.form = text;
        const auto form = parse_form(text);
        if (form) {
            if (const auto* found = world.find(*form)) {
                entry.type = found->type.to_string();
                entry.parent = found->parent.is_null() ? std::string("-")
                                                       : found->parent.to_string();
                entry.winner = order->entries()[found->winner].name;
                entry.owner = order->entries()[found->owner].name;
                entry.overrides = found->overrides;
                entry.flags = found->flags;
                entry.deleted = found->deleted;
                entry.injected = found->injected;
                wanted.emplace(form->value, facts.forms_pinned.size());
            }
        }
        facts.forms_pinned.push_back(std::move(entry));
    }

    PinSink sink(std::move(wanted), facts.forms_pinned);
    world.for_each_record(sink);
    facts.forwarded = sink.forwarded();

    if (!scratch.empty()) {
        const auto file_hash = [](const std::filesystem::path& path) -> std::optional<std::uint64_t> {
            auto mapped = io::MappedFile::open(path);
            if (!mapped) {
                return std::nullopt;
            }
            return fnv1a(mapped->bytes());
        };
        const auto records = scratch / "records.fb";
        if (const auto written = pack::write_snapshot(world, *order, records)) {
            const auto reopened = pack::Snapshot::open(records);
            const auto hash = file_hash(records);
            if (reopened && hash) {
                facts.snapshot = {{"forms", written->forms},
                                  {"payload_bytes", written->payload_bytes},
                                  {"editor_ids", written->editor_ids},
                                  {"types", written->types},
                                  {"bytes", std::filesystem::file_size(records)},
                                  {"hash", *hash}};
            }
        }
        const auto world_path = scratch / "world.fb";
        if (const auto w = pack::write_world(world, *order, world_path)) {
            const auto reopened = pack::WorldFile::open(world_path);
            const auto hash = file_hash(world_path);
            if (reopened && hash) {
                facts.world = {{"cells", w->cells},
                               {"interior_cells", w->interior_cells},
                               {"refs", w->refs},
                               {"doors", w->doors},
                               {"bases", w->bases},
                               {"lights", w->lights},
                               {"worlds", w->worlds},
                               {"terrains", w->terrains},
                               {"terrain_layers", w->terrain_layers},
                               {"land_textures", w->land_textures},
                               {"waters", w->waters},
                               {"climates", w->climates},
                               {"weathers", w->weathers},
                               {"scripts", w->scripts},
                               {"locks", w->locks},
                               {"links", w->links},
                               {"activate_parents", w->activate_parents},
                               {"primitives", w->primitives},
                               {"quests", w->quests},
                               {"quest_aliases", w->quest_aliases},
                               {"quest_fragments", w->quest_fragments},
                               {"globals", w->globals},
                               {"actors", w->actors},
                               {"unresolved", w->unresolved},
                               {"orphan_refs", w->orphan_refs},
                               {"parse_errors", w->parse_errors},
                               {"script_errors", w->script_errors},
                               {"bytes", w->file_bytes},
                               {"hash", *hash}};
            }
        }
    }
    return facts;
}

std::optional<FormsFacts> probe_forms(const std::filesystem::path& data_dir,
                                      const archive::ArchiveSet& sources,
                                      const std::vector<std::string>& plugins) {
    if (plugins.empty()) {
        return std::nullopt;
    }

    record::FormCensus census;
    FormsFacts facts;
    facts.types_defined = record::defined_types().size();

    for (const auto& name : plugins) {
        auto plugin = record::Plugin::open(data_dir / name);
        if (!plugin) {
            ++facts.unreadable;
            continue;
        }
        ++facts.plugins;

        // Localization is per plugin, as in `bethconv forms`.
        const bool localized = plugin->header().is_localized();
        census.set_localized(localized);
        facts.localized += localized ? 1 : 0;

        record::StringSource strings;
        if (localized) {
            // Same wiring as the merge.
            const auto fetch = [&sources](std::string_view vpath)
                -> std::optional<std::vector<std::byte>> {
                auto bytes = sources.read(vpath);
                if (!bytes) {
                    return std::nullopt;
                }
                return std::move(*bytes);
            };
            strings = record::load_string_source(fetch, plugin->name(),
                                                 record::k_default_language);
            if (!strings.empty()) {
                ++facts.string_tables;
                facts.table_entries += strings.size();
            }
        }
        census.set_strings(strings.empty() ? nullptr : &strings);
        (void)plugin->scan(census);
        // The tables are about to go out of scope; detach them.
        census.set_strings(nullptr);
    }

    facts.types_seen = census.types().size();
    facts.parsed = census.total_parsed();
    facts.failed = census.total_failed();
    facts.resolved_strings = census.total_resolved_strings();
    facts.unresolved_strings = census.total_unresolved_strings();

    for (const auto& [type, stats] : census.types()) {
        facts.leftover_fields += stats.leftover.size();
        for (const auto& occurrences : stats.leftover | std::views::values) {
            facts.leftover += occurrences;
        }
        facts.unhandled_fields += stats.unhandled.size();
        for (const auto& occurrences : stats.unhandled | std::views::values) {
            facts.unhandled += occurrences;
        }
    }
    facts.type_counts_hash = fnv1a(render_form_types(census.types()));
    facts.unhandled_hash = fnv1a(render_unhandled(census.types()));
    return facts;
}

ScriptFacts probe_scripts(const archive::ArchiveSet& sources) {
    std::vector<std::string> vpaths;
    sources.for_each([&](const archive::Resolution& entry) {
        if (entry.vpath.ends_with(".pex")) {
            vpaths.push_back(entry.vpath);
        }
    });
    std::ranges::sort(vpaths);
    ScriptFacts facts;
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const auto& vpath : vpaths) {
        ++facts.scripts;
        auto bytes = sources.read(vpath);
        auto decoded = bytes ? script::read_pex_script(*bytes, vpath)
                             : io::ParseResult<script::PexScript>(std::unexpected(bytes.error()));
        if (!decoded) {
            ++facts.failed;
            continue;
        }
        const auto count = [&](const script::PexFunction& f) {
            ++facts.functions;
            facts.natives += f.is_native() ? 1U : 0U;
            facts.instructions += f.opcodes.size();
        };
        for (const auto& o : decoded->objects) {
            ++facts.objects;
            for (const auto& p : o.properties) {
                if (p.getter) {
                    count(*p.getter);
                }
                if (p.setter) {
                    count(*p.setter);
                }
            }
            for (const auto& state : o.states) {
                for (const auto& f : state.functions) {
                    count(f);
                }
            }
        }
        for (const auto b : pack::write_script_asset(*decoded)) {
            hash = (hash ^ static_cast<std::uint8_t>(b)) * 0x100000001b3ULL;
        }
    }
    facts.asset_hash = hash;
    return facts;
}

} // namespace bethconv::corpus
