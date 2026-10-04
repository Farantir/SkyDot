// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"
#include "front_end.hpp"

#include <CLI/CLI.hpp>

#include <memory>

namespace bethconv::cli {
namespace {

struct DetectArgs {
    bool json = false;
};

} // namespace

void register_detect(CLI::App& app) {
    auto args = std::make_shared<DetectArgs>();
    auto* detect = app.add_subcommand("detect", "Find Skyrim installs and their plugins.txt");
    detect->add_flag("--json", args->json, "One JSON document on stdout");
    detect->callback([args] { set_exit_status(cmd_detect(args->json)); });
}

} // namespace bethconv::cli
