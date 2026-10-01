// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/convert.hpp"

#include "bethconv/io/mapped_file.hpp"
#include "bethconv/pack/lod_asset.hpp"
#include "bethconv/pack/script_asset.hpp"
#include "bethconv/script/pex.hpp"
#include "bethconv/texture/dds.hpp"
#include "bethconv/texture/mip_tail.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <system_error>
#include <string>
#include <utility>

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

[[nodiscard]] std::optional<AssetKind> kind_of(std::string_view extension,
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
    return std::nullopt;
}

[[nodiscard]] PackFailure failure_from(std::string_view vpath, std::string_view stage,
                                       const io::ParseError& error) {
    return PackFailure{.vpath = std::string(vpath),
                       .stage = std::string(stage),
                       .kind = std::string(io::to_string(error.kind)),
                       .detail = error.to_string()};
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
std::string ConvertOptions::mesh_settings() const {
    return "mesh/13;" + flag("collision", mesh_read.read_collision) + ";" +
           flag("animations", mesh_read.read_animations) + ";" +
           flag("skinning", mesh_read.read_skinning) + ";" +
           flag("skip_empty", mesh_read.skip_empty_shapes) + ";" +
           flag("skip_editor_markers", mesh_read.skip_editor_markers) + ";" +
           flag("y_up", mesh_write.convert_to_y_up) + ";" +
           "scale=" + fixed_float(mesh_write.unit_scale) + ";" +
           flag("extras", mesh_write.write_extras) + ";" +
           "refs=" + std::to_string(static_cast<int>(mesh_write.texture_refs));
}

std::string ConvertOptions::texture_settings() const {
    return "texture/1;" + flag("mip_tail", fix_mip_tail);
}

std::string ConvertOptions::script_settings() const {
    return "script/2;decoded";
}

std::string ConvertOptions::lod_settings() const {
    return "lod/1;decoded";
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
    pack_options.prune_orphans = options.prune_orphans;
    pack_options.layout = options.layout;

    auto writer = PackWriter::create(options.out, std::move(pack_options));
    if (!writer) {
        return std::unexpected(writer.error());
    }

    ConvertResult result;
    result.sources = set.sources().size();
    result.unique_paths = set.unique_paths();

    PackManifest manifest;
    manifest.converter = options.converter;
    manifest.language = options.language;

    const auto report = [&](const std::string& phase, std::uint64_t done, std::uint64_t total) {
        if (options.progress) {
            options.progress(phase, done, total);
        }
    };

    // ---- records.fb ---------------------------------------------------
    if (options.write_records) {
        record::MergeOptions merge_options;
        merge_options.language = options.language;
        merge_options.strings = [&set](std::string_view vpath)
            -> std::optional<std::vector<std::byte>> {
            auto bytes = set.read(vpath);
            if (!bytes) {
                return std::nullopt;
            }
            return std::move(*bytes);
        };

        report("merge", 0, order.entries().size());
        const auto world = record::MergedWorld::build(order, merge_options);
        result.merge = world.stats();
        report("merge", order.entries().size(), order.entries().size());

        auto stats = write_snapshot(
            world, order, writer->records_path(),
            SnapshotOptions{.converter = options.converter, .language = options.language});
        if (!stats) {
            return std::unexpected(stats.error());
        }
        result.snapshot = *stats;

        // Hashed so manifest.json can be checked without re-running the merge.
        auto records_hash = hash_file(writer->records_path(), options.converter);
        manifest.records = RecordsRecord{.forms = stats->forms,
                                         .file_bytes = stats->file_bytes,
                                         .hash = records_hash.value_or(ContentHash{})};
        report("records", stats->forms, stats->forms);

        // world.fb needs its own merge pass: payload FormIDs are resolved
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

    // ---- the asset passes ---------------------------------------------
    std::uint64_t done = 0;
    for (const std::string& vpath : work) {
        ++done;
        if (options.progress_interval != 0 && done % options.progress_interval == 0) {
            report("assets", done, work.size());
        }

        const auto extension = extension_of_vpath(vpath);
        const auto kind = kind_of(extension, options);
        if (!kind) {
            // Not converted: counted in the manifest, never read.
            writer->defer(extension.empty() ? "<none>" : extension);
            continue;
        }

        auto bytes = set.read(vpath);
        if (!bytes) {
            writer->fail(failure_from(vpath, "read", bytes.error()));
            continue;
        }

        std::string source_name;
        if (const auto resolution = set.resolve(vpath)) {
            source_name = set.sources()[resolution->winner].name;
        }

        const auto slot = writer->reserve(vpath, *kind, *bytes, source_name);
        if (slot.already_present) {
            writer->reuse(slot);
            continue;
        }

        switch (*kind) {
        case AssetKind::mesh: {
            auto model = mesh::read_nif(*bytes, vpath, options.mesh_read);
            if (!model) {
                writer->fail(failure_from(vpath, "mesh", model.error()));
                break;
            }
            for (const auto& warning : model->warnings) {
                writer->warn(PackWarning{.vpath = vpath, .detail = warning});
            }
            auto glb = mesh::write_glb(*model, options.mesh_write);
            if (!glb) {
                writer->fail(failure_from(vpath, "mesh", glb.error()));
                break;
            }
            if (auto stored = writer->store(slot, *glb); !stored) {
                writer->fail(failure_from(vpath, "write", stored.error()));
            }
            break;
        }
        case AssetKind::texture: {
            auto info = texture::parse_dds(*bytes, vpath);
            if (!info) {
                writer->fail(failure_from(vpath, "texture", info.error()));
                break;
            }
            std::span<const std::byte> payload = *bytes;
            texture::TailFix fix;
            if (options.fix_mip_tail) {
                auto completed = texture::complete_mip_tail(*bytes, *info, vpath);
                if (!completed) {
                    writer->fail(failure_from(vpath, "texture", completed.error()));
                    break;
                }
                fix = std::move(*completed);
                if (fix.outcome == texture::TailOutcome::completed) {
                    payload = fix.data;
                }
                if (fix.dropped_bytes != 0) {
                    writer->warn(PackWarning{
                        .vpath = vpath,
                        .detail = std::to_string(fix.dropped_bytes) +
                                  " bytes past the declared surfaces were dropped"});
                }
            }
            if (auto stored = writer->store(slot, payload); !stored) {
                writer->fail(failure_from(vpath, "write", stored.error()));
            }
            break;
        }
        case AssetKind::script: {
            auto info = script::parse_pex(*bytes, vpath);
            if (!info) {
                writer->fail(failure_from(vpath, "script", info.error()));
                break;
            }
            if (!info->convertible()) {
                // Valid PEX for another game: reported, not packed.
                writer->fail(PackFailure{
                    .vpath = vpath,
                    .stage = "script",
                    .kind = std::string(io::to_string(io::ErrorKind::unsupported)),
                    .detail = "compiled for " + std::string(to_string(info->game)) +
                              " (gameID " + std::to_string(info->game_id) + "), not skyrim"});
                break;
            }
            auto decoded = script::read_pex_script(*bytes, vpath);
            if (!decoded) {
                writer->fail(failure_from(vpath, "script", decoded.error()));
                break;
            }
            if (auto stored = writer->store(slot, write_script_asset(*decoded)); !stored) {
                writer->fail(failure_from(vpath, "write", stored.error()));
            }
            break;
        }
        case AssetKind::lod: {
            auto decoded = read_lod_source(*bytes, extension, vpath);
            if (!decoded) {
                writer->fail(failure_from(vpath, "lod", decoded.error()));
                break;
            }
            if (decoded->trailing_bytes != 0) {
                writer->warn(PackWarning{.vpath = vpath,
                                         .detail = std::to_string(decoded->trailing_bytes) +
                                                   " bytes after the declared tree blocks skipped"});
            }
            if (auto stored = writer->store(slot, write_lod_asset(*decoded)); !stored) {
                writer->fail(failure_from(vpath, "write", stored.error()));
            }
            break;
        }
        }
    }
    report("assets", work.size(), work.size());

    // ---- the manifest -------------------------------------------------
    for (const auto& entry : order.entries()) {
        manifest.load_order.push_back(entry.name);
        std::error_code ec;
        const auto size = std::filesystem::file_size(entry.path, ec);
        // Plugins are always hashed: records.fb depends on exactly these bytes.
        manifest.sources.push_back(
            SourceRecord{.name = entry.name,
                         .kind = "plugin",
                         .bytes = ec ? 0 : size,
                         .hash = hash_file(entry.path, options.converter)});
    }
    for (const auto& source : set.sources()) {
        std::error_code ec;
        const auto size = std::filesystem::is_directory(source.path)
                              ? 0
                              : std::filesystem::file_size(source.path, ec);
        manifest.sources.push_back(
            SourceRecord{.name = source.name,
                         .kind = std::string(archive::to_string(source.kind)),
                         .bytes = ec ? 0 : size,
                         .hash = options.hash_archives && !std::filesystem::is_directory(source.path)
                                     ? hash_file(source.path, options.converter)
                                     : std::nullopt});
    }

    auto stats = writer->finish(manifest);
    if (!stats) {
        return std::unexpected(stats.error());
    }
    result.pack = *stats;
    for (const auto& failure : writer->failures()) {
        if (result.first_failures.size() >= 10) {
            break;
        }
        result.first_failures.push_back(failure);
    }
    return result;
}

} // namespace bethconv::pack
