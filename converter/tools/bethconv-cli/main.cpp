// SPDX-License-Identifier: GPL-3.0-or-later
//
// bethconv CLI: app setup and the list of subcommands. Each one lives in
// commands/<name>.cpp (see commands/commands.hpp); what they share is in
// common.hpp.
#include "commands/commands.hpp"
#include "common.hpp"

#include <CLI/CLI.hpp>

#include <string>

int main(int argc, char** argv) {
    using namespace bethconv::cli;

    CLI::App app{"bethconv - ahead-of-time Bethesda data converter"};
    app.set_version_flag("--version", std::string(BETHCONV_VERSION));
    app.require_subcommand(1);

    // The order of `bethconv --help`.
    register_script(app);
    register_animation(app);
    register_texture(app);
    register_loadorder(app);
    register_probe(app);
    register_records(app);
    register_forms(app);
    register_strings(app);
    register_merge(app);
    register_convert(app);
    register_detect(app);
    register_mo2(app);
    register_target(app);
    register_info(app);
    register_cell(app);
    register_view(app);
    register_scan(app);
    register_extract(app);
    register_mesh(app);

    CLI11_PARSE(app, argc, argv);
    return exit_status();
}
