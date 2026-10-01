// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/install/mount_plan.hpp"

#include <algorithm>
#include <map>
#include <tuple>

namespace bethconv::install {
namespace {

[[nodiscard]] char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), ascii_lower);
    return out;
}

/// .bsa and .ba2 files directly in `dir` (the game ignores subfolders), sorted.
[[nodiscard]] std::vector<std::filesystem::path> archives_in(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const auto ext = lower(it->path().extension().string());
        if (ext == ".bsa" || ext == ".ba2") {
            out.push_back(it->path());
        }
    }
    std::ranges::sort(out);
    return out;
}

[[nodiscard]] std::string stem_of(std::string_view name) {
    const auto dot = name.rfind('.');
    return lower(dot == std::string_view::npos ? name : name.substr(0, dot));
}

} // namespace

MountPlan plan_data_folder(const std::filesystem::path& data) {
    MountPlan plan;
    plan.archives = archives_in(data);
    plan.loose.push_back(data);
    plan.plugin_dirs.push_back(data);
    return plan;
}

MountPlan plan_mo2(const std::filesystem::path& data, const Mo2Instance& instance,
                   const Mo2Profile& profile) {
    MountPlan plan;

    // Load-order position of every loaded plugin, by lowercased stem. The base
    // game and DLC load first and are absent from MO2's plugins.txt.
    std::map<std::string, std::size_t, std::less<>> position;
    std::size_t next = 0;
    for (const auto name : record::implicit_masters()) {
        position.try_emplace(stem_of(name), next++);
    }
    for (const auto& plugin : profile.plugins.plugins) {
        if (profile.plugins.marks_active && !plugin.active) {
            continue;
        }
        position.try_emplace(stem_of(plugin.name), next++);
    }

    // Which plugin an archive belongs to: `<stem>` or `<stem> - <anything>`,
    // the longest stem winning ("Foo - Bar.esp" owns "Foo - Bar - Textures").
    const auto owner = [&](const std::string& archive_stem) -> std::optional<std::size_t> {
        if (const auto it = position.find(archive_stem); it != position.end()) {
            return it->second;
        }
        std::optional<std::size_t> best;
        std::size_t best_length = 0;
        for (auto at = archive_stem.find(" - "); at != std::string::npos;
             at = archive_stem.find(" - ", at + 1)) {
            if (const auto it = position.find(std::string_view(archive_stem).substr(0, at));
                it != position.end() && at > best_length) {
                best = it->second;
                best_length = at;
            }
        }
        return best;
    };

    struct Candidate {
        std::filesystem::path path;
        bool from_data = false;
        std::size_t plugin = 0;
        std::size_t mod = 0;
    };
    // By lowercased filename: a later (higher priority) provider replaces the
    // file, as in MO2's virtual Data folder.
    std::map<std::string, Candidate> chosen;
    for (const auto& path : archives_in(data)) {
        chosen[lower(path.filename().string())] = Candidate{.path = path, .from_data = true};
    }
    for (std::size_t m = 0; m < profile.mods.size(); ++m) {
        for (const auto& path : archives_in(profile.mods[m].dir)) {
            const auto name = path.filename().string();
            const auto plugin = owner(stem_of(name));
            if (!plugin) {
                plan.unloaded_archives.push_back(path);
                continue;
            }
            chosen[lower(name)] = Candidate{.path = path, .plugin = *plugin, .mod = m};
        }
    }

    std::vector<Candidate> from_data;
    std::vector<Candidate> from_mods;
    for (auto& [name, candidate] : chosen) {
        (candidate.from_data ? from_data : from_mods).push_back(std::move(candidate));
    }
    std::ranges::sort(from_data, {}, &Candidate::path);
    std::ranges::sort(from_mods, [](const Candidate& a, const Candidate& b) {
        return std::tie(a.plugin, a.mod, a.path) < std::tie(b.plugin, b.mod, b.path);
    });
    for (auto* list : {&from_data, &from_mods}) {
        for (auto& candidate : *list) {
            plan.archives.push_back(std::move(candidate.path));
        }
    }

    plan.loose.push_back(data);
    for (const auto& mod : profile.mods) {
        plan.loose.push_back(mod.dir);
    }
    std::error_code ec;
    if (std::filesystem::is_directory(instance.overwrite_dir, ec)) {
        plan.loose.push_back(instance.overwrite_dir);
    }
    plan.plugin_dirs = plan.loose;
    plan.plugins = profile.plugins;
    return plan;
}

std::vector<std::string> mount(archive::ArchiveSet& set, const MountPlan& plan,
                               const std::function<void(std::size_t, std::size_t)>& progress) {
    std::vector<std::string> failures;
    int priority = 0;
    const std::size_t total = plan.archives.size() + plan.loose.size();
    std::size_t done = 0;
    const auto record = [&](const std::filesystem::path& path,
                            const io::ParseResult<std::size_t>& result) {
        if (!result) {
            failures.push_back(path.string() + ": " + result.error().to_string());
        }
        if (progress) {
            progress(++done, total);
        }
    };
    for (const auto& path : plan.archives) {
        record(path, set.mount_archive(path, priority++));
    }
    for (const auto& path : plan.loose) {
        record(path, set.mount_loose(path, priority++));
    }
    return failures;
}

} // namespace bethconv::install
