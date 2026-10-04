// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"
#include "front_end.hpp"

#include <CLI/CLI.hpp>

#include <filesystem>
#include <memory>

namespace bethconv::cli {
namespace {

struct InfoArgs {
    std::filesystem::path pack;
    bool json = false;
};

} // namespace

void register_info(CLI::App& app) {
    auto args = std::make_shared<InfoArgs>();
    auto* info = app.add_subcommand("info", "Summarize a pack: inputs, size, stale blob bytes");
    info->add_option("pack", args->pack, "The pack directory")
        ->required()
        ->check(CLI::ExistingDirectory);
    info->add_flag("--json", args->json, "One JSON document on stdout");
    info->callback([args] { set_exit_status(cmd_info(args->pack, args->json)); });
}

} // namespace bethconv::cli
