// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/archive/vpath.hpp"
#include "bethconv/io/mapped_file.hpp"
#include "bethconv/io/span_stream.hpp"
#include "bethconv/texture/dds.hpp"
#include "bethconv/texture/mip_tail.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bethconv::cli {
namespace {

/// Texture sweep totals. Formats are counted by name, which doubles as a
/// census of an install.
struct TextureTally {
    std::size_t files = 0;
    std::size_t failed = 0;
    std::size_t completed = 0;
    std::size_t already_complete = 0;
    std::size_t single_level = 0;
    std::size_t unsupported = 0;
    std::size_t short_chains = 0;
    std::size_t cubemaps = 0;
    std::size_t volumes = 0;
    std::size_t dx10 = 0;
    std::size_t written = 0;
    std::size_t bytes_read = 0;
    std::size_t bytes_written = 0;
    std::size_t bytes_added = 0;
    std::size_t bytes_dropped = 0;
    std::map<std::string, std::size_t> formats;
};

/// Desktop textures are passed through, with short mip chains completed for
/// Godot. Reads headers, completes chains, copies bytes; never decodes.
int cmd_texture(const std::vector<std::filesystem::path>& sources,
                std::vector<std::string> vpaths, const std::filesystem::path& list_file,
                const std::string& filter, const std::filesystem::path& out_dir, bool inspect,
                bool no_fix, std::size_t limit, bool verbose, bool quiet) {
    // A list file lets thousands of paths share one mount (nine minutes one
    // process at a time vs. 4.3 s in one).
    if (!list_file.empty()) {
        bool ok = false;
        auto listed = read_vpath_list(list_file, ok);
        if (!ok) {
            return 1;
        }
        vpaths.insert(vpaths.end(), std::make_move_iterator(listed.begin()),
                      std::make_move_iterator(listed.end()));
    }

    bethconv::archive::ArchiveSet set;
    std::vector<std::filesystem::path> mounts;
    std::vector<std::filesystem::path> loose_textures;
    std::error_code ec;
    for (const auto& source : sources) {
        if (!std::filesystem::is_directory(source, ec) && source.extension() == ".dds") {
            loose_textures.push_back(source);
        } else {
            mounts.push_back(source);
        }
    }
    mount_all(set, mounts);

    std::vector<std::string> work = vpaths;
    if (work.empty() && !mounts.empty()) {
        set.for_each([&](const bethconv::archive::Resolution& entry) {
            if (limit != 0 && work.size() >= limit) {
                return;
            }
            if (!entry.vpath.ends_with(".dds")) {
                return;
            }
            if (!filter.empty() && entry.vpath.find(filter) == std::string::npos) {
                return;
            }
            work.push_back(entry.vpath);
        });
        // Sorted, as for meshes, so truncated runs are reproducible.
        std::ranges::sort(work);
    }

    TextureTally tally;
    const auto started = std::chrono::steady_clock::now();

    auto handle = [&](std::string_view origin, std::span<const std::byte> bytes,
                      const std::filesystem::path& out_name) {
        ++tally.files;
        tally.bytes_read += bytes.size();

        auto info = bethconv::texture::parse_dds(bytes, origin);
        if (!info) {
            ++tally.failed;
            std::fprintf(stderr, "error: %s\n", info.error().to_string().c_str());
            return;
        }
        ++tally.formats[info->layout.name];
        if (info->short_chain()) {
            ++tally.short_chains;
        }
        switch (info->kind) {
        case bethconv::texture::SurfaceKind::cubemap: ++tally.cubemaps; break;
        case bethconv::texture::SurfaceKind::volume: ++tally.volumes; break;
        case bethconv::texture::SurfaceKind::texture_2d: break;
        }
        if (info->dx10) {
            ++tally.dx10;
        }

        // With --no-fix, `fix` stays default, so the control run reports no
        // chain work.
        bethconv::texture::TailFix fix;
        if (!no_fix) {
            auto completed = bethconv::texture::complete_mip_tail(bytes, *info, origin);
            if (!completed) {
                ++tally.failed;
                std::fprintf(stderr, "error: %s\n", completed.error().to_string().c_str());
                return;
            }
            fix = std::move(*completed);
            switch (fix.outcome) {
            case bethconv::texture::TailOutcome::completed: ++tally.completed; break;
            case bethconv::texture::TailOutcome::already_complete:
                ++tally.already_complete;
                break;
            case bethconv::texture::TailOutcome::single_level: ++tally.single_level; break;
            case bethconv::texture::TailOutcome::unsupported: ++tally.unsupported; break;
            }
        }
        tally.bytes_added += fix.added_bytes;
        tally.bytes_dropped += fix.dropped_bytes;

        if ((inspect && !quiet) || verbose) {
            std::printf("%-58s %5ux%-5u %-8s %-7s %2u/%-2u levels  %s", std::string(origin).c_str(),
                        info->width, info->height, info->layout.name.c_str(),
                        std::string(to_string(info->kind)).c_str(), info->stored_levels,
                        info->full_chain_levels,
                        std::string(to_string(fix.outcome)).c_str());
            if (fix.outcome == bethconv::texture::TailOutcome::completed) {
                std::printf(" +%zu bytes", fix.added_bytes);
            }
            if (fix.dropped_bytes != 0) {
                std::printf(" (dropped %zu trailing bytes)", fix.dropped_bytes);
            }
            std::printf("\n");
        }

        if (inspect || out_dir.empty()) {
            return;
        }
        const bool rebuilt = fix.outcome == bethconv::texture::TailOutcome::completed;
        const std::span<const std::byte> payload = rebuilt ? std::span<const std::byte>(fix.data)
                                                           : bytes;
        std::string error;
        if (!bethconv::io::write_file(out_dir / out_name, payload, error)) {
            ++tally.failed;
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return;
        }
        ++tally.written;
        tally.bytes_written += payload.size();
    };

    for (const auto& path : loose_textures) {
        auto mapped = bethconv::io::MappedFile::open(path);
        if (!mapped) {
            ++tally.files;
            ++tally.failed;
            std::fprintf(stderr, "error: %s\n", mapped.error().to_string().c_str());
            continue;
        }
        handle(path.string(), mapped->bytes(), std::filesystem::path(path).filename());
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
        handle(vpath, *bytes, std::filesystem::path(target));
    }

    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!tally.formats.empty()) {
        std::printf("\nformats:");
        // Most common first.
        std::vector<std::pair<std::string, std::size_t>> by_count(tally.formats.begin(),
                                                                  tally.formats.end());
        std::ranges::sort(by_count, [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        for (const auto& [name, count] : by_count) {
            std::printf("  %s %zu", name.c_str(), count);
        }
        std::printf("\n");
    }
    std::printf("%zu files, %zu failed | %zu cubemap, %zu volume, %zu dx10 | "
                "%zu short chains",
                tally.files, tally.failed, tally.cubemaps, tally.volumes, tally.dx10,
                tally.short_chains);
    if (no_fix) {
        std::printf(" (left short: --no-fix)\n");
    } else {
        std::printf(" -> %zu completed, %zu already complete, %zu single-level, "
                    "%zu unsupported\n",
                    tally.completed, tally.already_complete, tally.single_level,
                    tally.unsupported);
    }
    std::printf("%.1f MiB read", static_cast<double>(tally.bytes_read) / (1024.0 * 1024.0));
    if (tally.written != 0) {
        std::printf(", %zu written (%.1f MiB, %zu bytes appended)", tally.written,
                    static_cast<double>(tally.bytes_written) / (1024.0 * 1024.0),
                    tally.bytes_added);
    }
    if (tally.bytes_dropped != 0) {
        std::printf(", %zu trailing bytes dropped", tally.bytes_dropped);
    }
    std::printf(" in %.1fs\n", elapsed);
    return tally.failed == 0 ? 0 : 1;
}

struct TextureArgs {
    std::vector<std::filesystem::path> sources;
    std::vector<std::string> vpaths;
    std::filesystem::path list;
    std::string filter;
    std::filesystem::path out;
    bool inspect = false;
    bool no_fix = false;
    std::size_t limit = 0;
    bool verbose = false;
    bool quiet = false;
    bool allow_slow_target = false;
};

} // namespace

void register_texture(CLI::App& app) {
    auto args = std::make_shared<TextureArgs>();
    auto* texture =
        app.add_subcommand("texture", "Pass DDS textures through, completing the mip chain");
    texture->add_option("--source", args->sources,
                        "BSA/BA2, loose directory, or .dds file; repeat, in load order")
        ->required()
        ->check(CLI::ExistingPath);
    texture->add_option("vpath", args->vpaths,
                        "Virtual paths to convert; omit to sweep every .dds in the set");
    texture->add_option("--from", args->list, "Read virtual paths from this file, one per line")
        ->check(CLI::ExistingFile);
    texture->add_option("--filter", args->filter, "Only sweep paths containing this substring");
    texture->add_option("-o,--out", args->out, "Write .dds files under this directory");
    texture->add_flag("--allow-slow-target", args->allow_slow_target, k_allow_slow_help);
    texture->add_flag("--inspect", args->inspect, "Census only; write nothing");
    texture->add_flag("--no-fix", args->no_fix,
                      "Copy verbatim; do not complete short mip chains");
    texture->add_option("--limit", args->limit, "Stop after this many files")->default_val(0);
    texture->add_flag("-v,--verbose", args->verbose, "Print a line per texture");
    texture->add_flag("-q,--quiet", args->quiet, "Summary and census only, no per-file lines");
    texture->callback([args] {
        if (!args->out.empty() && !output_target_ok(args->out, true, args->allow_slow_target)) {
            set_exit_status(2);
            return;
        }
        set_exit_status(cmd_texture(args->sources, args->vpaths, args->list, args->filter,
                                    args->out, args->inspect, args->no_fix, args->limit,
                                    args->verbose, args->quiet));
    });
}

} // namespace bethconv::cli
