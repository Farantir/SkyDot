// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/archive/vpath.hpp"
#include "bethconv/io/span_stream.hpp"

#include <CLI/CLI.hpp>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace bethconv::cli {
namespace {

bool write_bytes(const std::filesystem::path& out, std::span<const std::byte> data) {
    std::error_code ec;
    if (out.has_parent_path()) {
        std::filesystem::create_directories(out.parent_path(), ec);
    }
    std::string error;
    if (!bethconv::io::write_file(out, data, error)) {
        std::fprintf(stderr, "error: cannot write %s: %s\n", out.string().c_str(), error.c_str());
        return false;
    }
    return true;
}

int cmd_extract(const std::vector<std::filesystem::path>& paths,
                std::vector<std::string> vpaths, const std::filesystem::path& list_file,
                const std::filesystem::path& out, bool quiet) {
    if (!list_file.empty()) {
        bool ok = false;
        auto listed = read_vpath_list(list_file, ok);
        if (!ok) {
            return 1;
        }
        vpaths.insert(vpaths.end(), std::make_move_iterator(listed.begin()),
                      std::make_move_iterator(listed.end()));
    }
    if (vpaths.empty()) {
        std::fprintf(stderr, "error: no virtual paths given\n");
        return 1;
    }

    bethconv::archive::ArchiveSet set;
    mount_all(set, paths);

    // One path: -o is the file. Several: -o is a root mirroring the virtual
    // tree (what glTF texture URIs resolve against).
    const bool out_is_dir = vpaths.size() > 1;

    std::size_t ok_count = 0;
    std::size_t missing = 0;
    std::size_t bytes = 0;
    for (const std::string& vpath : vpaths) {
        auto data = set.read(vpath);
        if (!data) {
            std::fprintf(stderr, "error: %s\n", data.error().to_string().c_str());
            ++missing;
            continue;
        }
        ++ok_count;
        bytes += data->size();

        const auto resolution = set.resolve(vpath);
        if (!quiet) {
            std::printf("%s: %zu bytes from %s\n", resolution->vpath.c_str(), data->size(),
                        set.sources()[resolution->winner].name.c_str());
        }

        if (out.empty()) {
            continue;
        }
        if (out_is_dir && !bethconv::archive::is_safe_relative(resolution->vpath)) {
            std::fprintf(stderr, "error: %s: not a safe relative path, not written\n",
                         resolution->vpath.c_str());
            ++missing;
            continue;
        }
        const std::filesystem::path target =
            out_is_dir ? out / std::filesystem::path(resolution->vpath) : out;
        if (!write_bytes(target, *data)) {
            return 1;
        }
        if (!quiet) {
            std::printf("wrote %s\n", target.string().c_str());
        }
    }

    if (vpaths.size() > 1) {
        std::printf("%zu extracted, %zu missing, %.1f MiB\n", ok_count, missing,
                    static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return missing == 0 ? 0 : 1;
}

struct ExtractArgs {
    std::vector<std::filesystem::path> sources;
    std::vector<std::string> vpaths;
    std::filesystem::path list;
    std::filesystem::path out;
    bool allow_slow_target = false;
    bool quiet = false;
};

} // namespace

void register_extract(CLI::App& app) {
    auto args = std::make_shared<ExtractArgs>();
    auto* extract = app.add_subcommand("extract", "Read virtual paths out of the mounted set");
    // One value per occurrence, so it does not swallow the positional vpath.
    // Repeat --source per archive, in load order.
    extract->add_option("--source", args->sources,
                        "BSA/BA2 file or loose directory; repeat, in load order")
        ->required()
        ->allow_extra_args(false)
        ->check(CLI::ExistingPath);
    extract->add_option("vpath", args->vpaths, "Virtual paths, e.g. meshes/clutter/foo.nif");
    extract->add_option("--from", args->list, "Read virtual paths from this file, one per line")
        ->check(CLI::ExistingFile);
    extract->add_option("-o,--out", args->out,
                        "Write here: a file for one path, a directory root for several");
    extract->add_flag("--allow-slow-target", args->allow_slow_target, k_allow_slow_help);
    extract->add_flag("-q,--quiet", args->quiet, "Summary only, no per-file line");
    extract->callback([args] {
        const bool many = !args->list.empty() || args->vpaths.size() > 1;
        if (!args->out.empty() && !output_target_ok(args->out, many, args->allow_slow_target)) {
            set_exit_status(2);
            return;
        }
        set_exit_status(cmd_extract(args->sources, args->vpaths, args->list, args->out,
                                    args->quiet));
    });
}

} // namespace bethconv::cli
