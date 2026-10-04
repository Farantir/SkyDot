// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"
#include "front_end.hpp"

#include <CLI/CLI.hpp>

#include <filesystem>
#include <memory>

namespace bethconv::cli {
namespace {

struct TargetArgs {
    std::filesystem::path dir;
    bool json = false;
};

} // namespace

void register_target(CLI::App& app) {
    auto args = std::make_shared<TargetArgs>();
    auto* target = app.add_subcommand(
        "target", "Check a folder for a pack: storage, free space, whether it is a pack");
    target->add_option("dir", args->dir, "The folder (need not exist)")->required();
    target->add_flag("--json", args->json, "One JSON document on stdout");
    target->callback([args] { set_exit_status(cmd_target(args->dir, args->json)); });
}

} // namespace bethconv::cli
