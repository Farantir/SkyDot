// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/animation/hkx.hpp"
#include "bethconv/archive/archive_set.hpp"
#include "bethconv/pack/animation_asset.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bethconv::cli {
namespace {

int cmd_animation(const std::vector<std::filesystem::path>& sources, std::vector<std::string> vpaths,
                  const std::string& filter, int sample_track, bool list_failures) {
    bethconv::archive::ArchiveSet set;
    mount_all(set, sources);
    if (vpaths.empty()) {
        set.for_each([&](const bethconv::archive::Resolution& entry) {
            if (entry.vpath.ends_with(".hkx") &&
                (filter.empty() || entry.vpath.find(filter) != std::string::npos)) {
                vpaths.push_back(entry.vpath);
            }
        });
        std::ranges::sort(vpaths);
    }
    std::size_t files = 0;
    std::size_t failed = 0;
    std::size_t skeletons = 0;
    std::size_t clips = 0;
    std::size_t interleaved = 0;
    std::size_t frames = 0;
    std::size_t multi_block = 0;
    std::size_t notes = 0;
    std::size_t round_trip_mismatch = 0;
    std::uint64_t source_bytes = 0;
    std::uint64_t asset_bytes = 0;
    std::map<std::string, std::size_t> errors;
    std::map<std::string, std::size_t, std::less<>> classes;
    const auto started = std::chrono::steady_clock::now();
    for (const auto& vpath : vpaths) {
        auto bytes = set.read(vpath);
        ++files;
        if (!bytes) {
            ++failed;
            std::fprintf(stderr, "error: %s\n", bytes.error().to_string().c_str());
            continue;
        }
        source_bytes += bytes->size();
        auto file = bethconv::animation::read_hkx(*bytes, vpath);
        if (!file) {
            ++failed;
            ++errors[std::string(bethconv::io::to_string(file.error().kind))];
            if (list_failures) {
                std::fprintf(stderr, "error: %s\n", file.error().to_string().c_str());
            }
            continue;
        }
        for (const auto& [name, count] : file->classes) {
            classes[name] += 1;
        }
        skeletons += file->skeletons.size();
        for (const auto& clip : file->clips) {
            ++clips;
            interleaved += clip.encoding == bethconv::animation::ClipEncoding::interleaved ? 1U : 0U;
            frames += clip.frame_count;
            multi_block += clip.blocks.size() > 1 ? 1U : 0U;
            for (const auto& track : clip.annotations) {
                notes += track.annotations.size();
            }
        }
        const auto asset = bethconv::pack::write_animation_asset(*file);
        asset_bytes += asset.size();
        const auto back = bethconv::pack::read_animation_asset(asset, vpath);
        if (!back || back->clips.size() != file->clips.size() ||
            back->skeletons.size() != file->skeletons.size()) {
            ++round_trip_mismatch;
        }
        if (sample_track >= 0) {
            for (std::size_t c = 0; c < file->clips.size(); ++c) {
                const auto& clip = file->clips[c];
                std::printf("# %s clip %zu: %u frames, %u tracks\n", vpath.c_str(), c, clip.frame_count,
                            clip.transform_tracks);
                for (std::uint32_t f = 0; f < clip.frame_count; ++f) {
                    const auto t = bethconv::animation::sample(clip, static_cast<std::uint32_t>(sample_track), f);
                    std::printf("%u %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", f, static_cast<double>(t.translation[0]),
                                static_cast<double>(t.translation[1]), static_cast<double>(t.translation[2]),
                                static_cast<double>(t.rotation[0]), static_cast<double>(t.rotation[1]),
                                static_cast<double>(t.rotation[2]), static_cast<double>(t.rotation[3]));
                }
            }
        }
    }
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("%zu files, %zu failed | %zu skeletons, %zu clips (%zu interleaved, %zu with several "
                "blocks), %zu frames, %zu annotations | %.1f MiB read, %.1f MiB of assets, %zu round-trip "
                "mismatches | %.1f s\n",
                files, failed, skeletons, clips, interleaved, multi_block, frames, notes,
                static_cast<double>(source_bytes) / (1024.0 * 1024.0),
                static_cast<double>(asset_bytes) / (1024.0 * 1024.0), round_trip_mismatch, elapsed);
    for (const auto& [kind, count] : errors) {
        std::printf("  failed: %zu %s\n", count, kind.c_str());
    }
    std::printf("files per class:");
    for (const auto& [name, count] : classes) {
        if (name.starts_with("hka") || name == "hkRootLevelContainer" || name == "hkbCharacterStringData") {
            std::printf(" %s %zu", name.c_str(), count);
        }
    }
    std::printf("\n");
    return failed == 0 && round_trip_mismatch == 0 ? 0 : 1;
}

struct AnimationArgs {
    std::vector<std::filesystem::path> sources;
    std::vector<std::string> vpaths;
    std::string filter;
    int sample = -1;
    bool failures = false;
};

} // namespace

void register_animation(CLI::App& app) {
    auto args = std::make_shared<AnimationArgs>();
    auto* anim_cmd =
        app.add_subcommand("animation", "Decode Havok skeletons and animations (.hkx)");
    anim_cmd->add_option("--source", args->sources, "Archive or directory to mount (repeatable)")
        ->required();
    anim_cmd->add_option("vpath", args->vpaths, "Files to decode; default every .hkx");
    anim_cmd->add_option("--filter", args->filter, "Only paths containing this substring");
    anim_cmd->add_option("--sample", args->sample,
                         "Print every frame of this track (translation, rotation)");
    anim_cmd->add_flag("--failures", args->failures, "Print each failure");
    anim_cmd->callback([args] {
        set_exit_status(cmd_animation(args->sources, args->vpaths, args->filter, args->sample,
                                      args->failures));
    });
}

} // namespace bethconv::cli
