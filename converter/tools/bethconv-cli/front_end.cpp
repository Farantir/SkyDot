// SPDX-License-Identifier: GPL-3.0-or-later
#include "front_end.hpp"

#include "bethconv/install/mo2.hpp"
#include "bethconv/io/json_text.hpp"
#include "bethconv/pack/asset_store.hpp"
#include "bethconv/pack/vpath_index.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <unordered_set>

namespace bethconv::cli {

using nlohmann::ordered_json;
using io::path_text;

namespace {

/// Steam builds the corpus harness last passed against (tests/corpus): SE
/// since the 2026-08-28 patch (tests/corpus/README.md), LE and VR as installed
/// then. A different build is not wrong, only untested.
[[nodiscard]] std::string_view tested_build(install::Edition edition) noexcept {
    switch (edition) {
    case install::Edition::le: return "15039560";
    case install::Edition::se: return "24914197";
    case install::Edition::vr: return "2743427";
    case install::Edition::unknown: break;
    }
    return {};
}

[[nodiscard]] ordered_json path_or_null(const std::optional<std::filesystem::path>& path) {
    return path ? ordered_json(path_text(*path)) : ordered_json(nullptr);
}

[[nodiscard]] int fail(bool json, const std::string& message) {
    if (json) {
        // Free text: dump() replaces invalid UTF-8; newlines stay newlines.
        emit(ordered_json{{"json_version", k_json_version}, {"error", message}});
    } else {
        std::fprintf(stderr, "error: %s\n", message.c_str());
    }
    return 1;
}

/// The nearest existing folder at or above `path`, for free space.
[[nodiscard]] std::filesystem::path existing_ancestor(std::filesystem::path path) {
    std::error_code ec;
    path = std::filesystem::absolute(path, ec);
    while (!path.empty() && !std::filesystem::exists(path, ec)) {
        const auto parent = path.parent_path();
        if (parent == path) {
            break;
        }
        path = parent;
    }
    return path;
}

[[nodiscard]] std::optional<std::vector<std::byte>> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    for (std::size_t i = 0; i < chars.size(); ++i) {
        bytes[i] = static_cast<std::byte>(chars[i]);
    }
    return bytes;
}

[[nodiscard]] std::string human_bytes(std::uint64_t bytes) {
    char buffer[32]{};
    const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gib >= 1.0) {
        std::snprintf(buffer, sizeof(buffer), "%.1f GiB", gib);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f MiB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buffer;
}

} // namespace

std::string_view to_string(TargetLevel level) noexcept {
    switch (level) {
    case TargetLevel::ok: return "ok";
    case TargetLevel::warn: return "warn";
    case TargetLevel::refuse: return "refuse";
    }
    return "ok";
}

TargetVerdict check_target(const std::filesystem::path& out, bool many_files, bool allow) {
    TargetVerdict verdict;
    verdict.target = io::probe_output_target(out);
    if (!verdict.target || !verdict.target->slow_for_many_files()) {
        return verdict;
    }
    const std::string where = out.string() + " is on " + verdict.target->describe() + ": " +
                              (verdict.target->fuse
                                   ? "a FUSE filesystem (every file operation goes through one "
                                     "userspace process; NTFS via ntfs-3g is one)"
                                   : "a spinning or zoned disk");
    if (!many_files) {
        verdict.level = TargetLevel::warn;
        verdict.reason = where + ". Large sequential files only; expect it to be slow.";
    } else if (allow) {
        verdict.level = TargetLevel::warn;
        verdict.reason = "writing many small files to " + where +
                         ". Proceeding because of --allow-slow-target.";
    } else {
        verdict.level = TargetLevel::refuse;
        verdict.reason = where +
                         ".\nWriting many small files there can stall the whole mount. Write to "
                         "a local SSD instead, use --store blob for a pack, or pass "
                         "--allow-slow-target.";
    }
    return verdict;
}

void emit(const ordered_json& doc) {
    const auto line = doc.dump(-1, ' ', false, ordered_json::error_handler_t::replace);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

ordered_json install_json(const install::GameInstall& install,
                          const install::DetectOptions& options) {
    const auto candidates = install::plugins_txt_candidates(install, options);
    const auto tested = tested_build(install.edition);
    ordered_json doc{
        {"edition", install::to_string(install.edition)},
        {"edition_name", install::display_name(install.edition)},
        {"source", install.source},
        {"root", path_text(install.root)},
        {"data", path_text(install.data)},
        {"app_id", install.app_id},
        {"build_id", install.build_id},
        {"tested_build_id", tested},
        // null when nothing says which build it is.
        {"build_tested", install.build_id.empty() || tested.empty()
                             ? ordered_json(nullptr)
                             : ordered_json(install.build_id == tested)},
        {"plugins_txt", path_or_null(install.plugins_txt)},
        {"plugins_txt_expected",
         candidates.empty() ? ordered_json(nullptr) : ordered_json(path_text(candidates.front()))},
    };
    return doc;
}

int cmd_detect(bool json) {
    const auto options = install::default_detect_options();
    const auto installs = install::detect_installs(options);
    if (json) {
        auto roots = ordered_json::array();
        for (const auto& root : options.steam_roots) {
            roots.push_back(path_text(root));
        }
        auto list = ordered_json::array();
        for (const auto& found : installs) {
            list.push_back(install_json(found, options));
        }
        emit(ordered_json{{"json_version", k_json_version},
                          {"steam_roots", std::move(roots)},
                          {"local_app_data", path_text(options.local_app_data)},
                          {"installs", std::move(list)}});
        return 0;
    }
    for (const auto& root : options.steam_roots) {
        std::printf("steam root   %s\n", root.string().c_str());
    }
    if (installs.empty()) {
        std::printf("no Skyrim install found\n");
        return 0;
    }
    for (const auto& found : installs) {
        std::printf("\n%s (%s)\n", std::string(install::display_name(found.edition)).c_str(),
                    found.source.c_str());
        std::printf("  data        %s\n", found.data.string().c_str());
        if (!found.build_id.empty()) {
            const auto tested = tested_build(found.edition);
            std::printf("  build       %s%s\n", found.build_id.c_str(),
                        tested.empty() || tested == found.build_id
                            ? ""
                            : (" (untested; the corpus harness ran against " +
                               std::string(tested) + ")")
                                  .c_str());
        }
        std::printf("  plugins.txt %s\n", found.plugins_txt
                                              ? found.plugins_txt->string().c_str()
                                              : "none (start the game's launcher once to write one)");
    }
    return 0;
}

int cmd_mo2(const std::filesystem::path& dir, const std::string& profile_name, bool json) {
    auto instance = install::read_mo2_instance(dir);
    if (!instance) {
        return fail(json, instance.error().to_string());
    }
    auto profile = install::read_mo2_profile(*instance, profile_name);

    std::optional<std::filesystem::path> game_data;
    if (instance->game_path) {
        if (auto data = install::find_ignoring_case(*instance->game_path, "Data");
            data && install::find_ignoring_case(*data, "Skyrim.esm")) {
            game_data = *data;
        }
    }

    if (json) {
        auto profiles = ordered_json::array();
        for (const auto& name : instance->profiles) {
            profiles.push_back(io::json_text(name));
        }
        ordered_json doc{{"json_version", k_json_version},
                         {"dir", path_text(instance->dir)},
                         {"game_name", io::json_text(instance->game_name)},
                         {"edition", install::to_string(instance->edition)},
                         {"game_path_written", io::json_text(instance->game_path_written)},
                         {"game_path", path_or_null(instance->game_path)},
                         {"game_data", path_or_null(game_data)},
                         {"selected_profile", io::json_text(instance->selected_profile)},
                         {"profiles", std::move(profiles)},
                         {"mods_dir", path_text(instance->mods_dir)},
                         {"overwrite_dir", path_text(instance->overwrite_dir)}};
        if (profile) {
            std::size_t active = 0;
            for (const auto& plugin : profile->plugins.plugins) {
                active += (!profile->plugins.marks_active || plugin.active) ? 1U : 0U;
            }
            auto missing = ordered_json::array();
            for (const auto& name : profile->missing) {
                missing.push_back(io::json_text(name));
            }
            doc["profile"] = ordered_json{{"name", io::json_text(profile->name)},
                                          {"mods_enabled", profile->mods.size()},
                                          {"mods_disabled", profile->disabled},
                                          {"separators", profile->separators},
                                          {"unmanaged", profile->unmanaged},
                                          {"missing", std::move(missing)},
                                          {"plugins_file", path_text(profile->plugins_file)},
                                          {"plugins_listed", profile->plugins.plugins.size()},
                                          {"plugins_active", active}};
        } else {
            doc["profile"] = nullptr;
            doc["profile_error"] = profile.error().to_string();
        }
        emit(doc);
        return profile ? 0 : 1;
    }

    std::printf("%s: %s\n", instance->dir.string().c_str(), instance->game_name.c_str());
    std::printf("  game path   %s%s\n", instance->game_path_written.c_str(),
                instance->game_path ? "" : " (not on this machine: pass --data)");
    std::printf("  profiles   ");
    for (const auto& name : instance->profiles) {
        std::printf(" [%s]%s", name.c_str(), name == instance->selected_profile ? "*" : "");
    }
    std::printf("\n");
    if (!profile) {
        std::fprintf(stderr, "error: %s\n", profile.error().to_string().c_str());
        return 1;
    }
    std::printf("profile %s: %zu mods enabled, %zu disabled, %zu separators, %zu missing; %zu "
                "plugins listed (%s)\n",
                profile->name.c_str(), profile->mods.size(), profile->disabled,
                profile->separators, profile->missing.size(), profile->plugins.plugins.size(),
                profile->plugins_file.filename().string().c_str());
    for (const auto& name : profile->missing) {
        std::printf("  missing: %s\n", name.c_str());
    }
    return 0;
}

int cmd_target(const std::filesystem::path& dir, bool json) {
    const auto blob = check_target(dir, false, false);
    const auto loose = check_target(dir, true, false);

    std::error_code ec;
    const bool exists = std::filesystem::is_directory(dir, ec);
    const bool is_pack = exists && std::filesystem::is_regular_file(dir / "manifest.json", ec);
    bool empty = true;
    if (exists) {
        empty = std::filesystem::directory_iterator(dir, ec) == std::filesystem::directory_iterator();
    }
    std::optional<std::uint64_t> free_bytes;
    if (const auto space = std::filesystem::space(existing_ancestor(dir), ec); !ec) {
        free_bytes = space.available;
    }

    if (json) {
        ordered_json doc{{"json_version", k_json_version},
                         {"path", path_text(dir)},
                         {"exists", exists},
                         {"is_pack", is_pack},
                         {"empty", empty},
                         {"free_bytes", free_bytes ? ordered_json(*free_bytes) : ordered_json(nullptr)}};
        if (const auto& t = blob.target) {
            doc["filesystem"] = ordered_json{
                {"mount_point", path_text(t->mount_point)},
                {"fs_type", t->fs_type},
                {"source", io::json_text(t->source)},
                {"disk", t->disk},
                {"fuse", t->fuse},
                {"rotational", t->rotational ? ordered_json(*t->rotational) : ordered_json(nullptr)},
                {"zoned", t->zoned}};
        } else {
            doc["filesystem"] = nullptr;
        }
        doc["blob"] = ordered_json{{"verdict", to_string(blob.level)},
                                   {"reason", blob.reason}};
        doc["loose"] = ordered_json{{"verdict", to_string(loose.level)},
                                    {"reason", loose.reason}};
        emit(doc);
        return 0;
    }

    std::printf("%s\n", dir.string().c_str());
    std::printf("  %s\n", !exists  ? "does not exist yet"
                          : is_pack ? "an existing pack"
                          : empty   ? "empty"
                                    : "not empty, not a pack");
    if (blob.target) {
        std::printf("  on %s\n", blob.target->describe().c_str());
    }
    if (free_bytes) {
        std::printf("  %s free\n", human_bytes(*free_bytes).c_str());
    }
    std::printf("  blob:  %s%s%s\n", std::string(to_string(blob.level)).c_str(),
                blob.reason.empty() ? "" : " - ", blob.reason.c_str());
    std::printf("  loose: %s%s%s\n", std::string(to_string(loose.level)).c_str(),
                loose.reason.empty() ? "" : " - ", loose.reason.c_str());
    return 0;
}

int cmd_info(const std::filesystem::path& pack, bool json) {
    std::ifstream in(pack / "manifest.json", std::ios::binary);
    if (!in) {
        return fail(json, "not a pack: no manifest.json in " + pack.string());
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto manifest = ordered_json::parse(text, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object()) {
        return fail(json, "manifest.json is not a JSON object: " + pack.string());
    }

    // Disk use: the pack's own files (a loose store's asset folders too).
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(pack, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            ++files;
            bytes += it->file_size(ec);
        }
    }

    // Stale blob bytes: assets no vpath.idx entry names any more, left by
    // reconversions until `convert --prune`.
    ordered_json blob = nullptr;
    if (manifest.contains("store") && manifest["store"].value("layout", "") == "blob") {
        const auto index_bytes = read_bytes(pack / manifest["store"].value("index", "assets.idx"));
        auto vpaths = pack::VpathIndex::read(pack / "vpath.idx");
        if (index_bytes && vpaths) {
            if (auto index = pack::AssetIndex::parse(*index_bytes, "assets.idx")) {
                std::unordered_set<std::string> named;
                for (const auto& entry : vpaths->entries()) {
                    named.insert(entry.hex);
                }
                std::uint64_t live = 0;
                std::uint64_t live_entries = 0;
                for (const auto& entry : index->entries) {
                    if (named.contains(entry.hash.hex())) {
                        live += entry.size;
                        ++live_entries;
                    }
                }
                blob = ordered_json{{"bytes", index->blob_bytes},
                                    {"entries", index->entries.size()},
                                    {"live_entries", live_entries},
                                    {"live_bytes", live},
                                    {"stale_bytes", index->blob_bytes > live
                                                        ? index->blob_bytes - live
                                                        : 0}};
            }
        }
    }

    const auto plugins = manifest.contains("load_order") ? manifest["load_order"].size() : 0;
    if (json) {
        auto summary = manifest;
        summary.erase("source_hashes");  // hundreds of entries nobody shows
        summary.erase("load_order");
        emit(ordered_json{{"json_version", k_json_version},
                          {"path", path_text(pack)},
                          {"plugins", plugins},
                          {"manifest", std::move(summary)},
                          {"disk", ordered_json{{"files", files}, {"bytes", bytes}}},
                          {"blob", std::move(blob)}});
        return 0;
    }

    std::printf("%s\n", pack.string().c_str());
    std::printf("  pack format %d, %s\n", manifest.value("pack_format_version", 0),
                manifest.value("converter", std::string("?")).c_str());
    if (manifest.contains("input")) {
        const auto& input = manifest["input"];
        std::printf("  from %s %s%s%s\n", input.value("kind", std::string()).c_str(),
                    input.value("data", std::string()).c_str(),
                    input.contains("mo2_profile") ? ", profile " : "",
                    input.value("mo2_profile", std::string()).c_str());
    }
    std::printf("  %zu plugins, %llu files, %s on disk\n", plugins,
                static_cast<unsigned long long>(files), human_bytes(bytes).c_str());
    if (!blob.is_null()) {
        std::printf("  blob: %s, %s stale (convert --prune removes it)\n",
                    human_bytes(blob["bytes"].get<std::uint64_t>()).c_str(),
                    human_bytes(blob["stale_bytes"].get<std::uint64_t>()).c_str());
    }
    if (manifest.contains("report")) {
        std::printf("  %llu failed, %llu warnings (report.json)\n",
                    manifest["report"].value("failed", 0ULL),
                    manifest["report"].value("warnings", 0ULL));
    }
    return 0;
}

} // namespace bethconv::cli
