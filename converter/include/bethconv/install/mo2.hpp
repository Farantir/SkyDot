// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mod Organizer 2 instances, read as files. MO2 never installs mods into the
// game's Data folder: each mod stays in `mods/<name>/`, and a virtual
// filesystem merges them only for programs MO2 starts. A converter that reads
// Data sees the unmodded game, so the instance has to be read directly.
//
// Layout (observed in a Wabbajack-installed portable instance, MO2 2.5.1):
//
//   <instance>/ModOrganizer.ini        [General] gameName, gamePath,
//                                      selected_profile; [Settings] may move
//                                      the folders below (%BASE_DIR% = instance)
//   <instance>/mods/<mod>/             one folder per mod, laid out like Data
//   <instance>/overwrite/              files tools wrote; above every mod
//   <instance>/profiles/<p>/modlist.txt   "+mod" enabled, "-mod" disabled,
//                                      "*name" unmanaged (DLC); the FIRST line
//                                      has the HIGHEST priority. Names ending
//                                      "_separator" are list dividers.
//   <instance>/profiles/<p>/plugins.txt   "*Plugin.esp" active, in load order
//   <instance>/profiles/<p>/loadorder.txt every plugin, in load order
//
// INI values are QSettings-encoded: `@ByteArray(...)` around strings that are
// not plain ASCII words, `\\` for a backslash, and Windows paths throughout.
// A list installed on Windows and read on Linux keeps its Windows gamePath, so
// the caller may have to supply the game folder.
#pragma once

#include "bethconv/install/game_install.hpp"
#include "bethconv/io/parse_error.hpp"
#include "bethconv/record/load_order.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::install {

struct Mo2Instance {
    std::filesystem::path dir;
    std::string game_name;          ///< gameName: "Skyrim Special Edition", "Skyrim VR", ...
    Edition edition{};              ///< From game_name.
    std::string game_path_written;  ///< gamePath as the INI has it.
    /// gamePath on this machine: as written if it exists, else a folder of
    /// that name inside the instance (Wabbajack's "Stock Game").
    std::optional<std::filesystem::path> game_path;
    std::string selected_profile;
    std::filesystem::path mods_dir;
    std::filesystem::path profiles_dir;
    std::filesystem::path overwrite_dir;
    std::vector<std::string> profiles; ///< Sorted.
};

struct Mo2Mod {
    std::string name;
    std::filesystem::path dir;
};

struct Mo2Profile {
    std::string name;
    std::filesystem::path dir;
    /// Enabled mods with a folder, LOWEST priority first: the order to mount
    /// them in.
    std::vector<Mo2Mod> mods;
    std::size_t disabled{};
    std::size_t separators{};
    std::size_t unmanaged{};
    /// Enabled in modlist.txt but without a folder in mods/.
    std::vector<std::string> missing;
    /// The load order with activation marks.
    record::PluginList plugins;
    std::filesystem::path plugins_file;
};

/// One `key=value` line's value, decoded from QSettings' INI encoding.
[[nodiscard]] std::string decode_ini_value(std::string_view raw);

/// Edition from MO2's gameName.
[[nodiscard]] Edition edition_from_game_name(std::string_view name) noexcept;

/// A modlist.txt line.
struct ModlistEntry {
    char state{};  ///< '+', '-' or '*'.
    std::string name;
};

/// Lines of modlist.txt in file order (highest priority first). Comments,
/// blank lines and lines without a state character are dropped.
[[nodiscard]] std::vector<ModlistEntry> parse_modlist(std::string_view text);

/// Read `<dir>/ModOrganizer.ini` and list the profiles.
[[nodiscard]] io::ParseResult<Mo2Instance> read_mo2_instance(const std::filesystem::path& dir);

/// Read one profile; an empty name means the instance's selected profile.
[[nodiscard]] io::ParseResult<Mo2Profile> read_mo2_profile(const Mo2Instance& instance,
                                                           std::string_view name = {});

} // namespace bethconv::install
