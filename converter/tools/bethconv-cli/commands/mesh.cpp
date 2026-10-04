// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/archive/vpath.hpp"
#include "bethconv/io/mapped_file.hpp"
#include "bethconv/mesh/gltf_writer.hpp"
#include "bethconv/mesh/nif_reader.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace bethconv::cli {
namespace {

/// Bounds of `points` times `scale` (game units), before any node transform.
void print_bounds(const std::vector<bethconv::mesh::Vec3>& points, float scale) {
    if (points.empty()) {
        return;
    }
    std::array<float, 3> lo{points[0].x, points[0].y, points[0].z};
    std::array<float, 3> hi = lo;
    for (const auto& v : points) {
        const std::array<float, 3> p{v.x, v.y, v.z};
        for (std::size_t k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
    }
    std::printf("      bounds (%.1f %.1f %.1f) to (%.1f %.1f %.1f)\n",
                static_cast<double>(lo[0] * scale), static_cast<double>(lo[1] * scale),
                static_cast<double>(lo[2] * scale), static_cast<double>(hi[0] * scale),
                static_cast<double>(hi[1] * scale), static_cast<double>(hi[2] * scale));
}

/// Result of one NIF, for the summary and sweep totals.
struct MeshTally {
    std::size_t files = 0;
    std::size_t failed = 0;
    std::size_t primitives = 0;
    std::size_t vertices = 0;
    std::size_t triangles = 0;
    std::size_t skinned = 0;
    std::size_t collision_shapes = 0;
    std::size_t warnings = 0;
    std::size_t clips = 0;
    std::size_t channels = 0;
    std::size_t particle_systems = 0;
    std::size_t glb_bytes = 0;
    std::size_t le = 0;
    std::size_t se = 0;
};

/// Sources may be archives, loose dirs or plain .nif files, so a single
/// extracted file can be converted without a mount.
int cmd_mesh(const std::vector<std::filesystem::path>& sources,
             const std::vector<std::string>& vpaths, const std::string& filter,
             const std::filesystem::path& out_dir, bool inspect, std::size_t limit,
             bool no_collision, bool no_skinning, bool keep_z_up, float unit_scale,
             bool verbose) {
    bethconv::mesh::ReadOptions read_options;
    read_options.read_collision = !no_collision;
    read_options.read_skinning = !no_skinning;

    bethconv::mesh::WriteOptions write_options;
    write_options.convert_to_y_up = !keep_z_up;
    write_options.unit_scale = unit_scale;

    bethconv::archive::ArchiveSet set;
    std::vector<std::filesystem::path> mounts;
    std::vector<std::filesystem::path> loose_nifs;
    std::error_code ec;
    for (const auto& source : sources) {
        if (!std::filesystem::is_directory(source, ec) && source.extension() == ".nif") {
            loose_nifs.push_back(source);
        } else {
            mounts.push_back(source);
        }
    }
    mount_all(set, mounts);

    // Explicit vpaths, or every .nif in the mount, filtered.
    std::vector<std::string> work = vpaths;
    if (work.empty() && !mounts.empty()) {
        set.for_each([&](const bethconv::archive::Resolution& entry) {
            if (limit != 0 && work.size() >= limit) {
                return;
            }
            if (!entry.vpath.ends_with(".nif")) {
                return;
            }
            if (!filter.empty() && entry.vpath.find(filter) == std::string::npos) {
                return;
            }
            work.push_back(entry.vpath);
        });
        // for_each walks a hash index; sort so truncated sweeps are
        // reproducible.
        std::ranges::sort(work);
    }

    MeshTally tally;
    const auto started = std::chrono::steady_clock::now();

    auto handle = [&](std::string_view origin, std::span<const std::byte> bytes,
                      const std::filesystem::path& out_name) {
        ++tally.files;
        auto model = bethconv::mesh::read_nif(bytes, origin, read_options);
        if (!model) {
            ++tally.failed;
            std::fprintf(stderr, "error: %s\n", model.error().to_string().c_str());
            return;
        }

        std::size_t vertices = 0;
        std::size_t triangles = 0;
        for (const auto& prim : model->primitives) {
            vertices += prim.positions.size();
            triangles += prim.indices.size() / 3;
            if (prim.skin.has_value()) {
                ++tally.skinned;
            }
        }
        tally.primitives += model->primitives.size();
        tally.vertices += vertices;
        tally.triangles += triangles;
        tally.collision_shapes += model->collision.size();
        tally.warnings += model->warnings.size();
        tally.clips += model->animations.size();
        for (const auto& clip : model->animations) {
            tally.channels += clip.channels.size();
        }
        tally.particle_systems += model->particles.size();
        switch (bethconv::mesh::flavor_of(model->nif_stream)) {
        case bethconv::mesh::NifFlavor::le: ++tally.le; break;
        case bethconv::mesh::NifFlavor::se: ++tally.se; break;
        case bethconv::mesh::NifFlavor::unknown: break;
        }

        if (inspect || verbose) {
            std::printf("%s\n", std::string(origin).c_str());
            std::printf("  %s  stream=%u (%s)  nodes=%zu shapes=%zu materials=%zu "
                        "skins=%zu collision=%zu\n",
                        model->nif_version.c_str(), model->nif_stream,
                        std::string(to_string(bethconv::mesh::flavor_of(model->nif_stream)))
                            .c_str(),
                        model->nodes.size(), model->primitives.size(),
                        model->materials.size(), model->skins.size(),
                        model->collision.size());
            for (const auto& prim : model->primitives) {
                const auto& mat = model->materials[prim.material];
                std::printf("    %-40s v=%-6zu tri=%-6zu %s%s%s%s mat=%u %s\n",
                            prim.name.c_str(), prim.positions.size(),
                            prim.indices.size() / 3,
                            prim.normals.empty() ? "-" : "N",
                            prim.tangents.empty() ? "-" : "T",
                            prim.uvs.empty() ? "-" : "U",
                            prim.colors.empty() ? "-" : "C", mat.bs_shader_type,
                            mat.textures[0].c_str());
                if (verbose) {
                    print_bounds(prim.positions, 1.0f);
                }
            }
            for (const auto& shape : model->collision) {
                std::printf("    collision %-18s verts=%zu tris=%zu\n",
                            shape.block_name.c_str(), shape.vertices.size(),
                            shape.indices.size() / 3);
                if (verbose) {
                    print_bounds(shape.vertices, bethconv::mesh::k_havok_scale);
                }
            }
            for (const auto& clip : model->animations) {
                std::printf("    clip '%s'%s %.2f-%.2fs x%.2f, %zu channels\n",
                            clip.name.c_str(), clip.autoplay ? " autoplay" : "",
                            static_cast<double>(clip.start), static_cast<double>(clip.stop),
                            static_cast<double>(clip.frequency), clip.channels.size());
                if (verbose) {
                    for (const auto& ch : clip.channels) {
                        std::printf("      %-28s %-26s %zu keys\n",
                                    model->nodes[ch.node].name.c_str(), ch.property.c_str(),
                                    ch.times.size());
                    }
                }
            }
            for (const auto& ps : model->particles) {
                std::printf("    particles '%s' max=%u emitters=%zu%s%s\n",
                            model->nodes[ps.node].name.c_str(), ps.max_particles,
                            ps.emitters.size(), ps.world_space ? " world" : " local",
                            ps.strip ? " strip" : "");
                for (const auto& em : ps.emitters) {
                    std::printf("      emitter rate=%.1f/s life=%.2f±%.2f speed=%.1f "
                                "radius=%.1f\n",
                                static_cast<double>(em.birth_rate),
                                static_cast<double>(em.life_span),
                                static_cast<double>(em.life_span_variation),
                                static_cast<double>(em.speed), static_cast<double>(em.radius));
                }
            }
        }
        for (const auto& warning : model->warnings) {
            std::fprintf(stderr, "warning: %s: %s\n", std::string(origin).c_str(),
                         warning.c_str());
        }

        if (inspect || out_dir.empty()) {
            return;
        }
        const auto written =
            bethconv::mesh::write_glb_file(*model, out_dir / out_name, write_options);
        if (!written) {
            ++tally.failed;
            std::fprintf(stderr, "error: %s\n", written.error().to_string().c_str());
            return;
        }
        tally.glb_bytes += *written;
    };

    for (const auto& path : loose_nifs) {
        auto mapped = bethconv::io::MappedFile::open(path);
        if (!mapped) {
            ++tally.files;
            ++tally.failed;
            std::fprintf(stderr, "error: %s\n", mapped.error().to_string().c_str());
            continue;
        }
        handle(path.string(), mapped->bytes(),
               std::filesystem::path(path).filename().replace_extension(".glb"));
    }

    for (const auto& vpath : work) {
        auto bytes = set.read(vpath);
        if (!bytes) {
            ++tally.files;
            ++tally.failed;
            std::fprintf(stderr, "error: %s\n", bytes.error().to_string().c_str());
            continue;
        }
        const std::string target = bethconv::archive::normalize_vpath(vpath);
        if (!out_dir.empty() && !inspect && !bethconv::archive::is_safe_relative(target)) {
            ++tally.files;
            ++tally.failed;
            std::fprintf(stderr, "error: %s: not a safe relative path, not written\n",
                         vpath.c_str());
            continue;
        }
        std::filesystem::path out_name(target);
        out_name.replace_extension(".glb");
        handle(vpath, *bytes, out_name);
    }

    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::printf("\n%zu files (%zu LE, %zu SE), %zu failed, %zu shapes, %zu vertices, "
                "%zu triangles, %zu skinned, %zu collision shapes, %zu clips, %zu channels, "
                "%zu particle systems, %zu warnings",
                tally.files, tally.le, tally.se, tally.failed, tally.primitives,
                tally.vertices, tally.triangles, tally.skinned, tally.collision_shapes,
                tally.clips, tally.channels, tally.particle_systems, tally.warnings);
    if (tally.glb_bytes != 0) {
        std::printf(", %.1f MiB written",
                    static_cast<double>(tally.glb_bytes) / (1024.0 * 1024.0));
    }
    std::printf(" in %.1fs\n", elapsed);
    return tally.failed == 0 ? 0 : 1;
}

struct MeshArgs {
    std::vector<std::filesystem::path> sources;
    std::vector<std::string> vpaths;
    std::string filter;
    std::filesystem::path out;
    bool inspect = false;
    std::size_t limit = 0;
    bool no_collision = false;
    bool no_skinning = false;
    bool keep_z_up = false;
    float unit_scale = 0.0142875f;
    bool verbose = false;
    bool allow_slow_target = false;
};

} // namespace

void register_mesh(CLI::App& app) {
    auto args = std::make_shared<MeshArgs>();
    auto* mesh = app.add_subcommand("mesh", "Convert NIFs to glTF, or report what is in them");
    mesh->add_option("--source", args->sources,
                     "BSA/BA2, loose directory, or .nif file; repeat, in load order")
        ->required()
        ->allow_extra_args(false)
        ->check(CLI::ExistingPath);
    mesh->add_option("vpath", args->vpaths,
                     "Virtual paths to convert; omit to sweep every .nif in the set");
    mesh->add_option("--filter", args->filter, "Only sweep paths containing this substring");
    mesh->add_option("-o,--out", args->out, "Write .glb files under this directory");
    mesh->add_flag("--allow-slow-target", args->allow_slow_target, k_allow_slow_help);
    mesh->add_flag("--inspect", args->inspect, "Report contents only; write nothing");
    mesh->add_option("--limit", args->limit, "Stop after this many files")->default_val(0);
    mesh->add_flag("--no-collision", args->no_collision, "Skip bhkCollisionObject extraction");
    mesh->add_flag("--no-skinning", args->no_skinning, "Skip skin/joint extraction");
    mesh->add_flag("--keep-z-up", args->keep_z_up, "Leave the model in NIF axes");
    mesh->add_option("--unit-scale", args->unit_scale,
                     "Metres per game unit; 1 leaves game units alone")
        ->default_val(0.0142875f);
    mesh->add_flag("-v,--verbose", args->verbose, "Print per-file detail while converting");
    mesh->callback([args] {
        if (!args->out.empty() && !output_target_ok(args->out, true, args->allow_slow_target)) {
            set_exit_status(2);
            return;
        }
        set_exit_status(cmd_mesh(args->sources, args->vpaths, args->filter, args->out,
                                 args->inspect, args->limit, args->no_collision,
                                 args->no_skinning, args->keep_z_up, args->unit_scale,
                                 args->verbose));
    });
}

} // namespace bethconv::cli
