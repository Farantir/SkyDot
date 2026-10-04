// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a conversion's inputs are: the mounts (a Data folder, Data under a Mod
// Organizer 2 profile, or an explicit list), the plugin list, the load order
// with the Creation Club plugins, and the InputRecord for the manifest. Every
// front end needs exactly this, so it is here and not in the CLI; a front end
// prints what comes back.
//
// A bad MO2 instance, profile or list file fails the call. A plugin that does
// not open is a load-order problem and a source that does not mount is a
// mount failure; neither stops anything (docs/format-notes/load-order.md).
#pragma once

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/install/mo2.hpp"
#include "bethconv/install/mount_plan.hpp"
#include "bethconv/io/parse_error.hpp"
#include "bethconv/pack/pack_writer.hpp"
#include "bethconv/record/load_order.hpp"
#include "bethconv/record/strings.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bethconv::pack {

struct InputSpec {
    /// The game's Data folder. Plugins are found here, and under an MO2 profile
    /// it is the bottom layer.
    std::filesystem::path data_dir{};

    /// A plugins.txt or loadorder.txt. Replaces the profile's list. Empty: the
    /// profile's, or without one the Data folder's plugins by file time.
    std::filesystem::path list_file{};

    /// A Mod Organizer 2 instance folder; empty for a plain Data folder.
    std::filesystem::path mo2{};

    /// With `mo2`: the profile; empty means the one the instance has selected.
    std::string mo2_profile{};

    /// Mount these instead of the Data folder's own, in order (a later one wins
    /// ties). Ignored with `mo2`.
    std::vector<std::filesystem::path> sources{};
};

/// Called after each source of a plan is mounted. Not called for
/// `InputSpec::sources`, which are few.
using MountProgress = std::function<void(std::size_t done, std::size_t total)>;

struct PreparedInputs {
    /// What was mounted, in order; nullopt for `InputSpec::sources`.
    std::optional<install::MountPlan> plan;

    /// MO2 only: the profile the plan came from, for "N mods enabled, M missing".
    std::optional<install::Mo2Profile> profile;

    record::LoadOrder order;
    archive::ArchiveSet set;

    /// Sources that did not mount, each "<name>: <why>"; the rest is mounted.
    std::vector<std::string> mount_failures;

    /// Mod archives no loaded plugin is named like: the game would not load
    /// them, so they are not mounted (install/mount_plan.hpp).
    std::vector<std::filesystem::path> unloaded_archives;

    /// For `ConvertOptions::input`.
    InputRecord input;
};

[[nodiscard]] io::ParseResult<PreparedInputs> prepare_inputs(const InputSpec& spec,
                                                             const MountProgress& progress = {});

/// Mount every archive and loose directory in `paths`, in order; a later mount
/// wins ties. Returns the ones that failed, "<filename>: <why>".
[[nodiscard]] std::vector<std::string> mount_sources(archive::ArchiveSet& set,
                                                     std::span<const std::filesystem::path> paths);

/// Adapt an archive set to strings.hpp's fetch callback, so the record layer
/// does not depend on the archive layer. `set` must outlive the callback.
[[nodiscard]] record::StringFetch string_fetch(const archive::ArchiveSet& set);

} // namespace bethconv::pack
