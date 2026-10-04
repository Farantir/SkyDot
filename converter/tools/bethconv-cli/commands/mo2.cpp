// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"
#include "front_end.hpp"

#include <CLI/CLI.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace bethconv::cli {
namespace {

struct Mo2Args {
    std::filesystem::path dir;
    std::string profile;
    bool json = false;
};

} // namespace

void register_mo2(CLI::App& app) {
    auto args = std::make_shared<Mo2Args>();
    auto* mo2 = app.add_subcommand("mo2", "Read a Mod Organizer 2 instance and one profile");
    mo2->add_option("instance", args->dir, "The instance folder (holds ModOrganizer.ini)")
        ->required()
        ->check(CLI::ExistingDirectory);
    mo2->add_option("--profile", args->profile, "Profile to read (default: the selected one)");
    mo2->add_flag("--json", args->json, "One JSON document on stdout");
    mo2->callback([args] { set_exit_status(cmd_mo2(args->dir, args->profile, args->json)); });
}

} // namespace bethconv::cli
