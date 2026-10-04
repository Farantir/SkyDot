// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/io/mapped_file.hpp"

#include <CLI/CLI.hpp>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

namespace bethconv::cli {
namespace {

int cmd_probe(const std::vector<std::filesystem::path>& paths) {
    int failures = 0;
    for (const auto& path : paths) {
        auto mapped = bethconv::io::MappedFile::open(path);
        if (!mapped) {
            std::fprintf(stderr, "error: %s\n", mapped.error().to_string().c_str());
            ++failures;
            continue;
        }
        auto reader = mapped->reader();
        const auto tag = reader.tag();
        std::printf("%-44s %12zu bytes  tag=%s\n", path.filename().string().c_str(),
                    mapped->size(), tag ? tag->to_string().c_str() : "<empty>");
    }
    return failures == 0 ? 0 : 1;
}

struct ProbeArgs {
    std::vector<std::filesystem::path> files;
};

} // namespace

void register_probe(CLI::App& app) {
    auto args = std::make_shared<ProbeArgs>();
    auto* probe = app.add_subcommand("probe", "Map files and print size and leading tag");
    probe->add_option("files", args->files, "Files to inspect")
        ->required()
        ->check(CLI::ExistingFile);
    probe->callback([args] { set_exit_status(cmd_probe(args->files)); });
}

} // namespace bethconv::cli
