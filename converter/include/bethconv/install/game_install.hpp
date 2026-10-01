// SPDX-License-Identifier: GPL-3.0-or-later
//
// Finding Skyrim installs and the files that describe them, so a front end can
// offer them instead of asking for paths.
//
//   - Steam: every library in `steamapps/libraryfolders.vdf`, then
//     `appmanifest_<appid>.acf` for the install folder and build id. App ids:
//     72850 Skyrim (2011), 489830 Special Edition (and Anniversary), 611670 VR.
//   - Windows: Steam's path and the "Bethesda Softworks" install keys from the
//     registry.
//   - `plugins.txt`: the game keeps it in `%LOCALAPPDATA%\<folder>\`, not in
//     the game folder; under Proton that folder is inside the app's prefix,
//     `steamapps/compatdata/<appid>/pfx/drive_c/users/steamuser/AppData/Local`.
//     Proton prefixes have been seen with `Plugins.txt`, so names are matched
//     case-insensitively.
//
// Nothing here reads game data beyond file names; an install is something with
// a Data folder holding Skyrim.esm.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::install {

enum class Edition : std::uint8_t { unknown, le, se, vr };

/// "le", "se", "vr", "unknown": stable, for JSON.
[[nodiscard]] std::string_view to_string(Edition edition) noexcept;

/// "Skyrim (2011)", "Skyrim Special Edition", "Skyrim VR".
[[nodiscard]] std::string_view display_name(Edition edition) noexcept;

/// Steam app id, or empty for unknown.
[[nodiscard]] std::string_view steam_app_id(Edition edition) noexcept;

struct GameInstall {
    Edition edition{};
    std::string source;          ///< "steam", "registry" or "folder".
    std::filesystem::path root;  ///< The game folder, holding the executable.
    std::filesystem::path data;  ///< `root / "Data"`.
    std::string app_id;          ///< Steam app id, if from Steam.
    std::string build_id;        ///< Steam build id, if from Steam.
    std::filesystem::path library; ///< Steam library holding it, if from Steam.
    /// The game's own plugins.txt for this install, if one exists.
    std::optional<std::filesystem::path> plugins_txt;
};

struct DetectOptions {
    /// Steam client folders (each holds `steamapps/`).
    std::vector<std::filesystem::path> steam_roots;
    /// Windows `%LOCALAPPDATA%`; empty elsewhere.
    std::filesystem::path local_app_data;
    /// Game folders found outside Steam (on Windows, from the registry).
    std::vector<std::filesystem::path> game_dirs;
};

/// Steam roots, `%LOCALAPPDATA%` and registry entries for this machine.
/// Missing places are left out, never an error.
[[nodiscard]] DetectOptions default_detect_options();

/// Every install found through `options`, Steam first, without duplicates.
[[nodiscard]] std::vector<GameInstall> detect_installs(const DetectOptions& options);

/// The edition of a game folder or Data folder: the executable decides
/// (TESV.exe, SkyrimSE.exe, SkyrimVR.exe), then SkyrimVR.esm.
[[nodiscard]] Edition identify(const std::filesystem::path& game_or_data);

/// An install for a folder the user picked: the game folder or its Data
/// folder. nullopt if there is no Skyrim.esm.
[[nodiscard]] std::optional<GameInstall> inspect_folder(const std::filesystem::path& dir,
                                                        const DetectOptions& options);

/// Where the game would keep plugins.txt for this install, most likely first,
/// whether or not the file exists.
[[nodiscard]] std::vector<std::filesystem::path> plugins_txt_candidates(
    const GameInstall& install, const DetectOptions& options);

/// Creation Club plugins the game loads for the install holding `data`, in
/// load order: `Skyrim.ccc` in the game folder (Special Edition; LE and VR have
/// none), one file name per line. Empty if there is no such file.
[[nodiscard]] std::vector<std::string> creation_club_plugins(const std::filesystem::path& data);

/// The entry of `dir` whose name equals `name` ignoring ASCII case, or nullopt.
[[nodiscard]] std::optional<std::filesystem::path> find_ignoring_case(
    const std::filesystem::path& dir, std::string_view name);

} // namespace bethconv::install
