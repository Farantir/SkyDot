// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/convert.hpp"

#include "alpha_usage.hpp"
#include "asset_conversion.hpp"
#include "ordered_pool.hpp"

#include "bethconv/animation/animation_data.hpp"
#include "bethconv/io/mapped_file.hpp"
#include "bethconv/pack/inputs.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <system_error>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bethconv::pack {
namespace {

/// Format a float for a settings fingerprint identically on every machine.
/// `%.9g` round-trips binary32; iostreams would use the locale, and a comma
/// decimal would rename every asset.
[[nodiscard]] std::string fixed_float(float value) {
    std::array<char, 32> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%.9g",
                                      static_cast<double>(value));
    if (written <= 0) {
        return "nan";
    }
    return std::string(buffer.data(), static_cast<std::size_t>(written));
}

[[nodiscard]] std::string flag(std::string_view name, bool value) {
    return std::string(name) + "=" + (value ? "1" : "0");
}

/// Extension including the dot, or empty. The vpath is already lowercase.
[[nodiscard]] std::string_view extension_of_vpath(std::string_view vpath) {
    const auto dot = vpath.rfind('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    const auto slash = vpath.rfind('/');
    if (slash != std::string_view::npos && dot < slash) {
        return {};
    }
    return vpath.substr(dot);
}

[[nodiscard]] std::optional<AssetKind> kind_of(std::string_view vpath, std::string_view extension,
                                               const ConvertOptions& options) {
    if (extension == ".nif" && options.convert_meshes) {
        return AssetKind::mesh;
    }
    if (extension == ".dds" && options.convert_textures) {
        return AssetKind::texture;
    }
    if (extension == ".pex" && options.convert_scripts) {
        return AssetKind::script;
    }
    // LOD meshes are NIFs.
    if ((extension == ".btr" || extension == ".bto") && options.convert_lod &&
        options.convert_meshes) {
        return AssetKind::mesh;
    }
    if ((extension == ".btt" || extension == ".lst" || extension == ".lod") && options.convert_lod) {
        return AssetKind::lod;
    }
    if ((extension == ".hkx" || animation::is_animation_data(vpath)) && options.convert_animations) {
        return AssetKind::animation;
    }
    return std::nullopt;
}

/// Hash a file on disk for the manifest, without holding it.
[[nodiscard]] std::optional<ContentHash> hash_file(const std::filesystem::path& path,
                                                   std::string_view converter) {
    auto mapped = io::MappedFile::open(path);
    if (!mapped) {
        return std::nullopt;
    }
    // No settings: a source hash names the input and must not change with
    // conversion options.
    return content_hash(mapped->bytes(), converter, "source");
}

/// Hash-table key over the digest: BLAKE3 output is already uniform.
struct DigestHasher {
    [[nodiscard]] std::size_t operator()(const ContentHash& hash) const noexcept {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            value |= std::to_integer<std::uint64_t>(hash.bytes[i]) << (8 * i);
        }
        return static_cast<std::size_t>(value);
    }
};

/// Which assets an input need not convert, for threads that run ahead of the
/// writer: the pack had them before the run, or an input earlier in the work
/// list has them. It is only a saving. The writer decides again, in work-list
/// order, with the store itself, and converts what was skipped for an earlier
/// input that then failed.
class Claims {
public:
    /// `stored`: the assets the pack had before the run.
    explicit Claims(const std::vector<ContentHash>& stored) {
        owner_.reserve(stored.size());
        for (const auto& hash : stored) {
            owner_.emplace(hash, 0);
        }
    }

    /// True if the pack or an input before `index` has this asset. Otherwise
    /// `index` becomes its owner: a lower index claiming later takes over, so
    /// the first input in the list always converts and the later ones skip.
    [[nodiscard]] bool taken(const ContentHash& hash, std::size_t index) {
        const std::lock_guard lock(mutex_);
        const auto [it, inserted] = owner_.try_emplace(hash, index + 1);
        if (inserted) {
            return false;
        }
        if (it->second <= index) {
            return true;
        }
        it->second = index + 1;
        return false;
    }

private:
    std::mutex mutex_;
    /// Owner's index plus one; 0 for assets from before the run.
    std::unordered_map<ContentHash, std::size_t, DigestHasher> owner_;
};

/// One work item after everything that needs no pack writer: read, hashed and,
/// unless its asset was taken, converted.
struct Prepared {
    std::string_view extension; ///< Of the work item's path.
    std::optional<AssetKind> kind; ///< Empty: this pass does not convert it.
    std::optional<PackFailure> read_failure;
    std::string source_name;
    ContentHash hash;
    std::uint64_t source_bytes{};
    /// The source, kept only when the asset is those bytes (a texture that
    /// needed no edit); every other asset is `converted->bytes`.
    std::vector<std::byte> source;
    /// Empty if the asset was taken.
    std::optional<AssetConversion> converted;

    /// What this holds, for the pool's memory budget.
    [[nodiscard]] std::uint64_t weight() const noexcept {
        return source.size() + (converted ? converted->bytes.size() : 0);
    }
};

[[nodiscard]] AssetConversion convert_bytes(AssetKind kind, std::span<const std::byte> bytes,
                                            std::string_view vpath, std::string_view extension,
                                            const ConvertOptions& options,
                                            unsigned encode_threads,
                                            std::optional<std::uint32_t> alpha_threshold) {
    switch (kind) {
    case AssetKind::mesh:
        return convert_mesh(bytes, vpath, options);
    case AssetKind::texture:
        return convert_texture(bytes, vpath, options, encode_threads, alpha_threshold);
    case AssetKind::script:
        return convert_script(bytes, vpath);
    case AssetKind::lod:
        return convert_lod(bytes, vpath, extension);
    case AssetKind::animation:
        return convert_animation(bytes, vpath);
    }
    return {};
}

/// The alpha-test threshold a texture's coverage mips are kept at, if its
/// materials gave it one.
[[nodiscard]] std::optional<std::uint32_t> threshold_of(const AlphaThresholds* thresholds,
                                                        AssetKind kind, const std::string& vpath) {
    if (thresholds == nullptr || kind != AssetKind::texture) {
        return std::nullopt;
    }
    const auto found = thresholds->find(vpath);
    if (found == thresholds->end()) {
        return std::nullopt;
    }
    return found->second;
}

/// Read, hash and convert one work item. `taken(hash, index)` says whether its
/// asset needs no conversion. Touches the writer only to hash, so any thread
/// may run it.
template <typename Taken>
[[nodiscard]] Prepared prepare(const archive::ArchiveSet& set, const PackWriter& writer,
                               const ConvertOptions& options, unsigned encode_threads,
                               const AlphaThresholds* thresholds, const std::string& vpath,
                               std::size_t index, Taken&& taken) {
    Prepared out;
    out.extension = extension_of_vpath(vpath);
    out.kind = kind_of(vpath, out.extension, options);
    if (!out.kind) {
        // Not converted: counted in the manifest, never read.
        return out;
    }
    auto bytes = set.read(vpath);
    if (!bytes) {
        out.read_failure = failure_from(vpath, "read", bytes.error());
        return out;
    }
    if (const auto resolution = set.resolve(vpath)) {
        out.source_name = set.sources()[resolution->winner].name;
    }
    const auto threshold = threshold_of(thresholds, *out.kind, vpath);
    out.hash = threshold ? writer.hash_of(*out.kind, *bytes,
                                                  coverage_recipe(*threshold, options.alpha_coverage))
                         : writer.hash_of(*out.kind, *bytes);
    out.source_bytes = bytes->size();
    if (taken(out.hash, index)) {
        return out;
    }
    out.converted =
        convert_bytes(*out.kind, *bytes, vpath, out.extension, options, encode_threads, threshold);
    if (out.converted->passthrough) {
        out.source = std::move(*bytes);
    }
    return out;
}

constexpr unsigned k_max_jobs = 64;

/// The threads to use for `items` inputs: the request, else every core.
[[nodiscard]] unsigned effective_jobs(unsigned requested, std::size_t items) {
    const unsigned wanted = requested != 0 ? requested : std::thread::hardware_concurrency();
    const unsigned capped = std::clamp(wanted, 1U, k_max_jobs);
    return static_cast<unsigned>(std::clamp<std::size_t>(items, 1, capped));
}

/// The asset passes: every input in `work`, in order, into `writer`. With more
/// than one job the reading, hashing and converting run ahead on a pool and
/// the writer takes the results in order; the writer's calls, and so the pack,
/// are the same as with one.
void convert_assets(const archive::ArchiveSet& set, const ConvertOptions& options,
                    const AlphaThresholds* thresholds, const std::vector<std::string>& work,
                    PackWriter& writer, ConvertResult& result) {
    const unsigned jobs = effective_jobs(options.jobs, work.size());
    result.jobs = jobs;
    // Several conversions at once already use every core; the block
    // compression need not add threads of its own.
    const unsigned encode_threads = jobs > 1 ? 1U : 0U;

    std::optional<Claims> claims;
    std::optional<OrderedPool<Prepared>> pool;
    if (jobs > 1) {
        claims.emplace(writer.stored_hashes());
        pool.emplace(
            work.size(), jobs, OrderedPool<Prepared>::Limits{},
            [&](std::size_t index) {
                return prepare(set, writer, options, encode_threads, thresholds, work[index], index,
                               [&](const ContentHash& hash, std::size_t at) {
                                   return claims->taken(hash, at);
                               });
            },
            [](const Prepared& item) { return item.weight(); });
    }

    std::uint64_t done = 0;
    for (std::size_t index = 0; index < work.size(); ++index) {
        const std::string& vpath = work[index];
        ++done;
        if (options.progress && options.progress_interval != 0 &&
            done % options.progress_interval == 0) {
            options.progress("assets", done, work.size());
        }

        Prepared item = pool ? pool->next()
                             : prepare(set, writer, options, encode_threads, thresholds, vpath, index,
                                       [&](const ContentHash& hash, std::size_t) {
                                           return writer.contains(hash);
                                       });
        if (!item.kind) {
            writer.defer(item.extension.empty() ? "<none>" : item.extension);
            continue;
        }
        if (item.read_failure) {
            writer.fail(std::move(*item.read_failure));
            continue;
        }

        const auto slot =
            writer.reserve(vpath, *item.kind, item.hash, item.source_bytes, item.source_name);
        if (slot.already_present) {
            writer.reuse(slot);
            continue;
        }
        if (!item.converted) {
            // Skipped for an earlier input with the same bytes, which did not
            // store its asset (its conversion failed). A single thread would
            // have converted this one too.
            auto bytes = set.read(vpath);
            if (!bytes) {
                writer.fail(failure_from(vpath, "read", bytes.error()));
                continue;
            }
            item.converted = convert_bytes(*item.kind, *bytes, vpath, item.extension, options, 0,
                                           threshold_of(thresholds, *item.kind, vpath));
            if (item.converted->passthrough) {
                item.source = std::move(*bytes);
            }
        }

        AssetConversion& converted = *item.converted;
        for (auto& warning : converted.warnings) {
            writer.warn(std::move(warning));
        }
        result.textures_shrunk += converted.textures.shrunk;
        result.textures_kept_large += converted.textures.kept_large;
        result.texture_bytes_saved += converted.textures.bytes_saved;
        result.textures_encoded += converted.textures.encoded;
        result.textures_not_encoded += converted.textures.not_encoded;
        result.alpha_coverage.adjusted += converted.textures.coverage_adjusted;
        result.alpha_coverage.unchanged += converted.textures.coverage_unchanged;
        result.alpha_coverage.single_level += converted.textures.coverage_single_level;
        result.alpha_coverage.unsupported += converted.textures.coverage_unsupported;
        result.alpha_coverage.raised += converted.textures.coverage_raised;
        result.alpha_coverage.lowered += converted.textures.coverage_lowered;
        if (converted.failure) {
            writer.fail(std::move(*converted.failure));
            continue;
        }
        if (auto stored = writer.store(slot, converted.asset(item.source)); !stored) {
            writer.fail(failure_from(vpath, "write", stored.error()));
        }
    }
    if (options.progress) {
        options.progress("assets", work.size(), work.size());
    }
}

} // namespace

/// The leading version must be bumped whenever the writer's output changes for
/// unchanged input; otherwise rebuilds reuse stale assets. 1 -> 2: escaping of
/// bytes JSON and RFC 3986 forbid in texture paths. 3: non-finite floats kept
/// out of JSON. 4: EditorMarker geometry dropped, shader flags in extras,
/// refraction surfaces transparent. 5: effect falloff and emissive alpha in
/// extras. 6: billboard modes. 7: tangents along U. 8: animations, particle
/// systems, hidden nodes and node ids. 9: every drag modifier. 10: rigid body
/// quality, mass, friction and restitution; nested collision transforms
/// composed. 11: compressed-mesh triangles listed after a chunk's strips.
/// 12: cylinder, strips and plane collision shapes. 13: sky shaders' texture.
/// 14: inverse bind matrices include the skinned shape's own placement.
/// 15: quadratic keys' in and out tangents the right way round. 16: hair and
/// skin tint colours in the material extras. 17: BSOrderedNode children's draw
/// order. 18: BSWaterShaderProperty named instead of "other". 19: AddOnNode
/// indices in the node extras.
std::string ConvertOptions::mesh_settings() const {
    return "mesh/19;" + flag("collision", mesh_read.read_collision) + ";" +
           flag("animations", mesh_read.read_animations) + ";" +
           flag("skinning", mesh_read.read_skinning) + ";" +
           flag("skip_empty", mesh_read.skip_empty_shapes) + ";" +
           flag("skip_editor_markers", mesh_read.skip_editor_markers) + ";" +
           flag("y_up", mesh_write.convert_to_y_up) + ";" +
           "scale=" + fixed_float(mesh_write.unit_scale) + ";" +
           flag("extras", mesh_write.write_extras) + ";" +
           "refs=" + std::to_string(static_cast<int>(mesh_write.texture_refs));
}

std::string_view to_string(AlphaCoverage mode) noexcept {
    switch (mode) {
    case AlphaCoverage::off: return "off";
    case AlphaCoverage::floor: return "floor";
    case AlphaCoverage::exact: return "exact";
    }
    return "?";
}

std::optional<AlphaCoverage> alpha_coverage_from_string(std::string_view text) noexcept {
    for (const auto mode : {AlphaCoverage::off, AlphaCoverage::floor, AlphaCoverage::exact}) {
        if (text == to_string(mode)) {
            return mode;
        }
    }
    return std::nullopt;
}

std::string ConvertOptions::texture_settings() const {
    // The limit only when set, so full-size packs keep their asset names.
    // The alpha coverage pass adds its threshold to the recipe of the textures
    // it treats (alpha_usage.hpp), so it is not part of this fingerprint and
    // the other textures keep their names.
    return "texture/1;" + flag("mip_tail", fix_mip_tail) +
           (max_texture_size != 0 ? ";max=" + std::to_string(max_texture_size) : std::string()) +
           (texture_encoding != texture::Encoding::keep
                ? ";encode=" + std::string(texture::to_string(texture_encoding))
                : std::string());
}

std::string ConvertOptions::script_settings() const {
    return "script/2;decoded";
}

std::string ConvertOptions::lod_settings() const {
    return "lod/1;decoded";
}

std::string ConvertOptions::animation_settings() const {
    // 2: behaviour characters and clip generators; animationdata text files.
    return "animation/2;decoded";
}

io::ParseResult<ConvertResult> convert(const archive::ArchiveSet& set,
                                       const record::LoadOrder& order,
                                       const ConvertOptions& options) {
    PackOptions pack_options;
    pack_options.converter = options.converter;
    pack_options.mesh_settings = options.mesh_settings();
    pack_options.texture_settings = options.texture_settings();
    pack_options.script_settings = options.script_settings();
    pack_options.lod_settings = options.lod_settings();
    pack_options.animation_settings = options.animation_settings();
    pack_options.prune_orphans = options.prune_orphans;
    pack_options.layout = options.layout;

    auto writer = PackWriter::create(options.out, std::move(pack_options));
    if (!writer) {
        return std::unexpected(writer.error());
    }

    // Packs written by earlier converters carry a `records.fb`, which nothing
    // reads and this run's manifest no longer names. Rebuilding one drops it.
    std::error_code stale_ec;
    std::filesystem::remove(options.out / "records.fb", stale_ec);

    ConvertResult result;
    result.sources = set.sources().size();
    result.unique_paths = set.unique_paths();

    PackManifest manifest;
    manifest.converter = options.converter;
    manifest.language = options.language;
    manifest.input = options.input;
    if (options.convert_textures) {
        manifest.textures = TextureRecord{.max_size = options.max_texture_size,
                                          .complete_mip_chains = options.fix_mip_tail,
                                          .uncompressed = std::string(
                                              texture::to_string(options.texture_encoding))};
    }

    const auto report = [&](const std::string& phase, std::uint64_t done, std::uint64_t total) {
        if (options.progress) {
            options.progress(phase, done, total);
        }
    };

    // ---- world.fb -----------------------------------------------------
    if (options.write_world) {
        record::MergeOptions merge_options;
        merge_options.language = options.language;
        merge_options.strings = string_fetch(set);

        report("merge", 0, order.entries().size());
        const auto world = record::MergedWorld::build(order, merge_options);
        result.merge = world.stats();
        report("merge", order.entries().size(), order.entries().size());

        // write_world is the merge's second pass: payload FormIDs are resolved
        // through each winning plugin's master list.
        auto world_stats = write_world(world, order, writer->world_path());
        if (!world_stats) {
            return std::unexpected(world_stats.error());
        }
        result.world = *world_stats;
        auto world_hash = hash_file(writer->world_path(), options.converter);
        manifest.world = WorldRecord{.cells = world_stats->cells,
                                     .refs = world_stats->refs,
                                     .bases = world_stats->bases,
                                     .file_bytes = world_stats->file_bytes,
                                     .hash = world_hash.value_or(ContentHash{})};
        report("world", world_stats->cells, world_stats->cells);
    }

    // ---- the work list ------------------------------------------------
    // Sorted: `for_each` walks a hash index, and `--limit` and determinism need
    // a stable order.
    std::vector<std::string> work;
    set.for_each([&](const archive::Resolution& entry) {
        if (!options.filter.empty() && entry.vpath.find(options.filter) == std::string::npos) {
            return;
        }
        work.push_back(entry.vpath);
    });
    std::ranges::sort(work);
    if (options.limit != 0 && work.size() > options.limit) {
        work.resize(options.limit);
    }
    result.considered = work.size();

    // ---- which textures are alpha-tested ---------------------------------
    // Before the textures: their recipe (and so their asset name) depends on
    // how the meshes use them.
    AlphaUsage usage;
    const bool scan_alpha = options.alpha_coverage != AlphaCoverage::off && options.convert_textures;
    if (scan_alpha) {
        report("alpha-scan", 0, 1);
        usage = scan_alpha_usage(set, options.jobs);
        result.alpha_coverage = usage.summary;
        result.alpha_coverage.mode = std::string(to_string(options.alpha_coverage));
        report("alpha-scan", 1, 1);
    }

    // ---- the asset passes ---------------------------------------------
    convert_assets(set, options, scan_alpha ? &usage.thresholds : nullptr, work, *writer, result);
    if (scan_alpha) {
        manifest.alpha_coverage = result.alpha_coverage;
    }

    // ---- the manifest -------------------------------------------------
    // Hashing every plugin and writing the indexes (and pruning) takes seconds
    // on a large install; a phase of its own so progress does not stall.
    report("finish", 0, 1);
    for (const auto& entry : order.entries()) {
        manifest.load_order.push_back(entry.name);
        std::error_code ec;
        const auto size = std::filesystem::file_size(entry.path, ec);
        // Plugins are always hashed: world.fb depends on exactly these bytes.
        manifest.sources.push_back(
            SourceRecord{.name = entry.name,
                         .kind = "plugin",
                         .bytes = ec ? 0 : size,
                         .hash = hash_file(entry.path, options.converter)});
    }
    for (const auto& source : set.sources()) {
        std::error_code ec;
        // An unreadable path is not a directory, so file_size reports the
        // failure (size 0) and the hash comes back empty.
        const bool is_directory = std::filesystem::is_directory(source.path, ec);
        const auto size = is_directory ? 0 : std::filesystem::file_size(source.path, ec);
        manifest.sources.push_back(
            SourceRecord{.name = source.name,
                         .kind = std::string(archive::to_string(source.kind)),
                         .bytes = ec ? 0 : size,
                         .hash = options.hash_archives && !is_directory
                                     ? hash_file(source.path, options.converter)
                                     : std::nullopt});
    }

    auto stats = writer->finish(manifest);
    if (!stats) {
        return std::unexpected(stats.error());
    }
    result.pack = *stats;
    report("finish", 1, 1);
    for (const auto& failure : writer->failures()) {
        if (result.first_failures.size() >= 10) {
            break;
        }
        result.first_failures.push_back(failure);
    }
    return result;
}

} // namespace bethconv::pack
