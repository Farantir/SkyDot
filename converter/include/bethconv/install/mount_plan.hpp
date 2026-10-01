// SPDX-License-Identifier: GPL-3.0-or-later
//
// What to mount, in which order, for a plain Data folder or for Data plus a
// Mod Organizer 2 profile. The order follows the game's rules:
//
//   1. Archives below every loose file, whatever a mod's priority.
//   2. Among archives: the Data folder's (alphabetically, as before; vanilla
//      has no conflicts between them), then mod archives in the load order of
//      the plugin they belong to. The game loads a mod's archive only if it is
//      named after a loaded plugin (`<plugin>.bsa`, `<plugin> - Textures.bsa`);
//      any `<plugin> - *.bsa` is accepted here. Others are reported, not
//      mounted.
//   3. Loose files: Data, then each enabled mod lowest priority first, then
//      MO2's overwrite folder.
//
// Two mods shipping the same archive name resolve as MO2's virtual filesystem
// does: the higher priority mod's file is the only one the game sees.
#pragma once

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/install/mo2.hpp"
#include "bethconv/record/load_order.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bethconv::install {

struct MountPlan {
    /// Mount order; a later archive wins over an earlier one.
    std::vector<std::filesystem::path> archives;
    /// Mount order, above every archive; a later folder wins.
    std::vector<std::filesystem::path> loose;
    /// Where plugins are found; a later folder wins (record::LoadOrder::build).
    std::vector<std::filesystem::path> plugin_dirs;
    /// The load order, if the input has one (a profile always does).
    std::optional<record::PluginList> plugins;
    /// Mod archives no loaded plugin names: the game would not load them.
    std::vector<std::filesystem::path> unloaded_archives;
};

/// Every archive in `data` (sorted), then `data`'s loose files.
[[nodiscard]] MountPlan plan_data_folder(const std::filesystem::path& data);

/// The game's Data folder under an MO2 profile's mods and overwrite folder.
[[nodiscard]] MountPlan plan_mo2(const std::filesystem::path& data, const Mo2Instance& instance,
                                 const Mo2Profile& profile);

/// Mount the plan: archives, then loose folders, each with a higher priority
/// than the one before. Returns the sources that failed to mount with why.
/// `progress(done, total)` is called after each source: indexing hundreds of
/// mod folders on a slow disk takes most of a minute.
[[nodiscard]] std::vector<std::string> mount(
    archive::ArchiveSet& set, const MountPlan& plan,
    const std::function<void(std::size_t done, std::size_t total)>& progress = {});

} // namespace bethconv::install
