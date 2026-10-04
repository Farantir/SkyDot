// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/inputs.hpp"

#include "bethconv/install/game_install.hpp"
#include "bethconv/io/json_text.hpp"

#include <system_error>
#include <utility>

namespace bethconv::pack {

io::ParseResult<PreparedInputs> prepare_inputs(const InputSpec& spec,
                                               const MountProgress& progress) {
    PreparedInputs prepared;
    InputRecord& input = prepared.input;
    input.kind = spec.mo2.empty() ? "data" : "mo2";
    input.edition = std::string(install::to_string(install::identify(spec.data_dir)));
    input.data = io::path_text(spec.data_dir);

    // ---- what to mount ------------------------------------------------
    install::MountPlan plan;
    if (!spec.mo2.empty()) {
        auto instance = install::read_mo2_instance(spec.mo2);
        if (!instance) {
            return std::unexpected(std::move(instance).error());
        }
        auto profile = install::read_mo2_profile(*instance, spec.mo2_profile);
        if (!profile) {
            return std::unexpected(std::move(profile).error());
        }
        if (instance->edition != install::Edition::unknown) {
            input.edition = std::string(install::to_string(instance->edition));
        }
        input.mo2_instance = io::path_text(instance->dir);
        input.mo2_profile = profile->name;
        input.mods = profile->mods.size();
        input.plugin_list = io::path_text(profile->plugins_file);
        plan = install::plan_mo2(spec.data_dir, *instance, *profile);
        prepared.profile = std::move(*profile);
    } else if (spec.sources.empty()) {
        plan = install::plan_data_folder(spec.data_dir);
    }
    if (!spec.list_file.empty()) {
        auto list = record::read_plugin_list(spec.list_file);
        if (!list) {
            return std::unexpected(std::move(list).error());
        }
        plan.plugins = std::move(*list);
        input.plugin_list = io::path_text(spec.list_file);
    }

    // ---- the load order -----------------------------------------------
    if (plan.plugins) {
        auto dirs = plan.plugin_dirs;
        if (dirs.empty()) {
            dirs.push_back(spec.data_dir);
        }
        record::LoadOrderOptions order_options;
        order_options.always_loaded = install::creation_club_plugins(spec.data_dir);
        prepared.order = record::LoadOrder::build(dirs, *plan.plugins, order_options);
    } else {
        auto built = record::LoadOrder::from_directory(spec.data_dir);
        if (!built) {
            return std::unexpected(std::move(built).error());
        }
        prepared.order = std::move(*built);
    }

    // ---- mounting -----------------------------------------------------
    const bool explicit_sources = !spec.sources.empty() && spec.mo2.empty();
    if (explicit_sources) {
        prepared.mount_failures = mount_sources(prepared.set, spec.sources);
    } else {
        prepared.mount_failures = install::mount(prepared.set, plan, progress);
    }
    prepared.unloaded_archives = std::move(plan.unloaded_archives);
    if (!explicit_sources) {
        prepared.plan = std::move(plan);
    }
    return prepared;
}

std::vector<std::string> mount_sources(archive::ArchiveSet& set,
                                       std::span<const std::filesystem::path> paths) {
    std::vector<std::string> failures;
    int priority = 0;
    std::error_code ec;
    for (const auto& path : paths) {
        // An unreadable path is not a directory; mount_archive then says why.
        const auto count = std::filesystem::is_directory(path, ec)
                               ? set.mount_loose(path, priority)
                               : set.mount_archive(path, priority);
        if (!count) {
            failures.push_back(path.filename().string() + ": " + count.error().to_string());
        }
        ++priority;
    }
    return failures;
}

record::StringFetch string_fetch(const archive::ArchiveSet& set) {
    return [&set](std::string_view vpath) -> std::optional<std::vector<std::byte>> {
        auto bytes = set.read(vpath);
        if (!bytes) {
            return std::nullopt;
        }
        return std::move(*bytes);
    };
}

} // namespace bethconv::pack
