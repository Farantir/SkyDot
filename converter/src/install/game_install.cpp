// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/install/game_install.hpp"

#include "bethconv/install/vdf.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bethconv::install {
namespace {

struct EditionInfo {
    Edition edition;
    std::string_view app_id;
    std::string_view executable;
    /// `%LOCALAPPDATA%` folder names, Steam's first. The others are the GOG,
    /// Epic and Microsoft Store builds of Special Edition.
    std::array<std::string_view, 4> app_data;
};

constexpr std::array<EditionInfo, 3> k_editions{{
    {Edition::le, "72850", "TESV.exe", {"Skyrim", {}, {}, {}}},
    {Edition::se, "489830", "SkyrimSE.exe",
     {"Skyrim Special Edition", "Skyrim Special Edition GOG", "Skyrim Special Edition EPIC",
      "Skyrim Special Edition MS"}},
    {Edition::vr, "611670", "SkyrimVR.exe", {"Skyrim VR", {}, {}, {}}},
}};

[[nodiscard]] const EditionInfo* info_of(Edition edition) noexcept {
    for (const auto& info : k_editions) {
        if (info.edition == edition) {
            return &info;
        }
    }
    return nullptr;
}

[[nodiscard]] char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::ranges::equal(a, b, [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
}

[[nodiscard]] bool is_dir(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

[[nodiscard]] bool is_file(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

/// The path to compare installs by: canonical if it resolves, else as given.
[[nodiscard]] std::filesystem::path identity(const std::filesystem::path& path) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical;
}

[[nodiscard]] std::filesystem::path env_path(const char* name) {
    // getenv is fine here: read once, at startup, from one thread.
#ifdef _MSC_VER
#pragma warning(suppress : 4996)
#endif
    const char* value = std::getenv(name);
    return value != nullptr ? std::filesystem::path(value) : std::filesystem::path();
}

#ifdef _WIN32
[[nodiscard]] std::filesystem::path registry_path(HKEY root, const wchar_t* key,
                                                  const wchar_t* value) {
    DWORD bytes = 0;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t)) {
        return {};
    }
    std::wstring text(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, text.data(), &bytes) !=
        ERROR_SUCCESS) {
        return {};
    }
    text.resize(bytes / sizeof(wchar_t));
    while (!text.empty() && text.back() == L'\0') {
        text.pop_back();
    }
    return std::filesystem::path(text);
}
#endif

/// Libraries a Steam root knows about, the root itself first.
[[nodiscard]] std::vector<std::pair<std::filesystem::path, const VdfNode*>>
libraries_of(const std::filesystem::path& root, const VdfNode* folders) {
    std::vector<std::pair<std::filesystem::path, const VdfNode*>> out;
    out.emplace_back(root, nullptr);
    if (folders == nullptr) {
        return out;
    }
    for (const auto& entry : folders->children) {
        // Current format: "0" { "path" "..." "apps" {...} }. Before 2021:
        // "1" "D:\\SteamLibrary".
        if (entry.is_object) {
            const auto path = entry.get("path");
            if (!path.empty()) {
                out.emplace_back(std::filesystem::path(std::string(path)), entry.find("apps"));
            }
        } else if (!entry.value.empty() && std::ranges::all_of(entry.key, [](char c) {
                       return c >= '0' && c <= '9';
                   })) {
            out.emplace_back(std::filesystem::path(entry.value), nullptr);
        }
    }
    return out;
}

void add_install(std::vector<GameInstall>& out, GameInstall install,
                 const DetectOptions& options) {
    const auto id = identity(install.root);
    for (const auto& existing : out) {
        if (identity(existing.root) == id) {
            return;
        }
    }
    for (const auto& candidate : plugins_txt_candidates(install, options)) {
        if (is_file(candidate)) {
            install.plugins_txt = candidate;
            break;
        }
    }
    out.push_back(std::move(install));
}

} // namespace

std::string_view to_string(Edition edition) noexcept {
    switch (edition) {
    case Edition::le: return "le";
    case Edition::se: return "se";
    case Edition::vr: return "vr";
    case Edition::unknown: break;
    }
    return "unknown";
}

std::string_view display_name(Edition edition) noexcept {
    switch (edition) {
    case Edition::le: return "Skyrim (2011)";
    case Edition::se: return "Skyrim Special Edition";
    case Edition::vr: return "Skyrim VR";
    case Edition::unknown: break;
    }
    return "Skyrim (unknown edition)";
}

std::string_view steam_app_id(Edition edition) noexcept {
    const auto* info = info_of(edition);
    return info != nullptr ? info->app_id : std::string_view();
}

std::optional<std::filesystem::path> find_ignoring_case(const std::filesystem::path& dir,
                                                        std::string_view name) {
    const auto exact = dir / name;
    std::error_code ec;
    if (std::filesystem::exists(exact, ec)) {
        return exact;
    }
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (iequals(it->path().filename().string(), name)) {
            return it->path();
        }
    }
    return std::nullopt;
}

std::vector<std::string> creation_club_plugins(const std::filesystem::path& data) {
    std::vector<std::string> out;
    auto root = data.filename().empty() ? data.parent_path().parent_path() : data.parent_path();
    const auto file = find_ignoring_case(root, "Skyrim.ccc");
    if (!file) {
        return out;
    }
    std::ifstream in(*file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        const auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') {
            continue;
        }
        out.push_back(line.substr(start));
    }
    return out;
}

Edition identify(const std::filesystem::path& game_or_data) {
    auto root = game_or_data;
    if (iequals(root.filename().string(), "data")) {
        root = root.parent_path();
    } else if (root.filename().empty()) {
        root = root.parent_path();  // a trailing separator
        if (iequals(root.filename().string(), "data")) {
            root = root.parent_path();
        }
    }
    // VR first: its folder has no other executable, but SE's never has VR's.
    for (const auto edition : {Edition::vr, Edition::se, Edition::le}) {
        if (find_ignoring_case(root, info_of(edition)->executable)) {
            return edition;
        }
    }
    if (const auto data = find_ignoring_case(root, "Data")) {
        if (find_ignoring_case(*data, "SkyrimVR.esm")) {
            return Edition::vr;
        }
    }
    return Edition::unknown;
}

std::optional<GameInstall> inspect_folder(const std::filesystem::path& dir,
                                          const DetectOptions& options) {
    std::filesystem::path root = dir;
    std::filesystem::path data;
    if (find_ignoring_case(dir, "Skyrim.esm") && !find_ignoring_case(dir, "Data")) {
        data = dir;  // the Data folder itself
        root = dir.filename().empty() ? dir.parent_path().parent_path() : dir.parent_path();
    } else if (const auto found = find_ignoring_case(dir, "Data");
               found && find_ignoring_case(*found, "Skyrim.esm")) {
        data = *found;
    } else {
        return std::nullopt;
    }
    GameInstall install;
    install.edition = identify(root);
    install.source = "folder";
    install.root = root;
    install.data = data;
    for (const auto& candidate : plugins_txt_candidates(install, options)) {
        if (is_file(candidate)) {
            install.plugins_txt = candidate;
            break;
        }
    }
    return install;
}

std::vector<std::filesystem::path> plugins_txt_candidates(const GameInstall& install,
                                                          const DetectOptions& options) {
    std::vector<std::filesystem::path> out;
    const auto* info = info_of(install.edition);
    if (info == nullptr) {
        return out;
    }
    const auto add_folder = [&](const std::filesystem::path& local) {
        for (const auto folder : info->app_data) {
            if (folder.empty()) {
                continue;
            }
            const auto dir = local / folder;
            if (const auto found = find_ignoring_case(dir, "plugins.txt")) {
                out.push_back(*found);
            } else {
                out.push_back(dir / "plugins.txt");
            }
        }
    };
    // Proton keeps the game's AppData inside the app's prefix, in the library
    // that holds the game.
    if (!install.library.empty()) {
        add_folder(install.library / "steamapps" / "compatdata" / std::string(info->app_id) /
                   "pfx" / "drive_c" / "users" / "steamuser" / "AppData" / "Local");
    }
    if (!options.local_app_data.empty()) {
        add_folder(options.local_app_data);
    }
    return out;
}

DetectOptions default_detect_options() {
    DetectOptions options;
    std::vector<std::filesystem::path> roots;
#ifdef _WIN32
    roots.push_back(registry_path(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"));
    roots.push_back(
        registry_path(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"));
    roots.emplace_back("C:/Program Files (x86)/Steam");
    options.local_app_data = env_path("LOCALAPPDATA");
    // Written by the launchers of Steam and GOG builds alike.
    for (const auto* key : {L"SOFTWARE\\WOW6432Node\\Bethesda Softworks\\Skyrim",
                            L"SOFTWARE\\WOW6432Node\\Bethesda Softworks\\Skyrim Special Edition",
                            L"SOFTWARE\\WOW6432Node\\Bethesda Softworks\\Skyrim VR"}) {
        auto dir = registry_path(HKEY_LOCAL_MACHINE, key, L"installed path");
        if (!dir.empty()) {
            options.game_dirs.push_back(std::move(dir));
        }
    }
#else
    const auto home = env_path("HOME");
    auto data_home = env_path("XDG_DATA_HOME");
    if (data_home.empty() && !home.empty()) {
        data_home = home / ".local" / "share";
    }
    if (!data_home.empty()) {
        roots.push_back(data_home / "Steam");
    }
    if (!home.empty()) {
        roots.push_back(home / ".steam" / "steam");
        roots.push_back(home / ".steam" / "root");
        roots.push_back(home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" /
                        "Steam");
        roots.push_back(home / "snap" / "steam" / "common" / ".local" / "share" / "Steam");
#ifdef __APPLE__
        roots.push_back(home / "Library" / "Application Support" / "Steam");
#endif
    }
#endif
    for (const auto& root : roots) {
        if (root.empty() || !is_dir(root / "steamapps")) {
            continue;
        }
        const auto id = identity(root);
        if (std::ranges::none_of(options.steam_roots,
                                 [&](const auto& seen) { return identity(seen) == id; })) {
            options.steam_roots.push_back(root);
        }
    }
    return options;
}

std::vector<GameInstall> detect_installs(const DetectOptions& options) {
    std::vector<GameInstall> out;
    std::vector<std::filesystem::path> seen_libraries;
    for (const auto& root : options.steam_roots) {
        auto folders = read_vdf(root / "steamapps" / "libraryfolders.vdf");
        const VdfNode* list = nullptr;
        if (folders) {
            list = folders->find("libraryfolders");
        }
        for (const auto& [library, apps] : libraries_of(root, list)) {
            const auto id = identity(library);
            if (std::ranges::find(seen_libraries, id) != seen_libraries.end()) {
                continue;
            }
            seen_libraries.push_back(id);
            for (const auto& info : k_editions) {
                // The library lists its apps; skip the rest without touching
                // the disk, which may be slow or missing.
                if (apps != nullptr && apps->find(info.app_id) == nullptr) {
                    continue;
                }
                const auto manifest = library / "steamapps" /
                                      ("appmanifest_" + std::string(info.app_id) + ".acf");
                if (!is_file(manifest)) {
                    continue;
                }
                auto acf = read_vdf(manifest);
                if (!acf) {
                    continue;
                }
                const auto* state = acf->find("AppState");
                if (state == nullptr || state->get("installdir").empty()) {
                    continue;
                }
                GameInstall install;
                install.edition = info.edition;
                install.source = "steam";
                install.root = library / "steamapps" / "common" / std::string(state->get("installdir"));
                const auto data = find_ignoring_case(install.root, "Data");
                if (!data || !find_ignoring_case(*data, "Skyrim.esm")) {
                    continue;
                }
                install.data = *data;
                install.app_id = std::string(info.app_id);
                install.build_id = std::string(state->get("buildid"));
                install.library = library;
                add_install(out, std::move(install), options);
            }
        }
    }
    for (const auto& dir : options.game_dirs) {
        if (auto install = inspect_folder(dir, options)) {
            install->source = "registry";
            add_install(out, std::move(*install), options);
        }
    }
    return out;
}

} // namespace bethconv::install
