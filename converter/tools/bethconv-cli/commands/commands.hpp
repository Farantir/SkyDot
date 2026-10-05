// SPDX-License-Identifier: GPL-3.0-or-later
//
// One `register_<name>` per subcommand, each in commands/<name>.cpp: it adds
// the subcommand to `app`, binds its options, and sets the callback that runs
// the command and leaves its exit status (common.hpp). main.cpp calls them in
// the order `--help` lists them.
//
//   probe     map a file and report what it looks like
//   records   parse plugins, print a type/field histogram
//   forms     run the field definitions over plugins
//   scan      list archives/plugins and their conflicts
//   loadorder resolve plugins.txt into an indexed load order
//   strings   read a plugin's .STRINGS tables
//   merge     collapse a load order into one flat world
//   extract   pull virtual paths out of the archive set
//   mesh      convert NIFs to glTF, or report what is in them
//   texture   pass DDS through, completing the mip chain
//   script    decode compiled Papyrus scripts
//   animation decode Havok skeletons and animations
//   convert   produce a pack
//   view      materialize a pack as a directory tree
//   cell      inspect cells in a pack's world.fb
//   detect    find installs (front_end.cpp runs it, as the next three)
//   mo2       read a Mod Organizer 2 instance
//   target    check a folder for a pack
//   info      summarize a pack
#pragma once

#include <CLI/CLI.hpp>

namespace bethconv::cli {

void register_probe(CLI::App& app);
void register_records(CLI::App& app);
void register_forms(CLI::App& app);
void register_scan(CLI::App& app);
void register_loadorder(CLI::App& app);
void register_strings(CLI::App& app);
void register_merge(CLI::App& app);
void register_extract(CLI::App& app);
void register_mesh(CLI::App& app);
void register_texture(CLI::App& app);
void register_script(CLI::App& app);
void register_animation(CLI::App& app);
void register_convert(CLI::App& app);
void register_view(CLI::App& app);
void register_cell(CLI::App& app);
void register_detect(CLI::App& app);
void register_mo2(CLI::App& app);
void register_target(CLI::App& app);
void register_info(CLI::App& app);

} // namespace bethconv::cli
