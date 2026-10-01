// SPDX-License-Identifier: GPL-3.0-or-later
//
// Builds an ordered, indexed load order from a data folder and a list file,
// and maps plugin-local FormIDs to global ones.
//
// Measured over 622 real plugins (see docs/format-notes/load-order.md):
//
//   1. A FormID's high byte indexes the plugin's own master list; the plugin
//      itself is one past the end.
//   2. The 0xFE light space never appears on disk; it is assigned here.
//   3. The listed order is the load order. Moving masters first would create
//      violations (DynDOLOD.esm is an ESM that depends on ESPs), so nothing is
//      reordered and violations are reported. Only the implicit masters
//      (Skyrim.esm and DLC) are moved to the front, as the game does.
//
// Source for flags and the compact space:
// <https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format>.
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/record/plugin.hpp"
#include "bethconv/record/types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::record {

/// Highest normal mod index. 0xFE marks light plugins and 0xFF is the save
/// game's, so 254 normal plugins, not 255.
inline constexpr std::uint32_t k_max_normal_index = 0xFD;

/// Light plugins share the 0xFE prefix; a 12-bit index tells them apart and
/// leaves 12 bits of object index.
inline constexpr std::uint32_t k_light_prefix = 0xFE00'0000;
inline constexpr std::uint32_t k_max_light_index = 0xFFF;
inline constexpr std::uint32_t k_light_object_mask = 0x0000'0FFF;
inline constexpr std::uint32_t k_normal_object_mask = 0x00FF'FFFF;

/// Plugins the game always loads, in this order.
///
/// MO2 omits them from `plugins.txt`, which would shift every index by five.
/// `SkyrimVR.esm` is included; what exists on disk decides.
[[nodiscard]] std::span<const std::string_view> implicit_masters() noexcept;

/// One line of `plugins.txt` or `loadorder.txt`.
struct ListedPlugin {
    std::string name;
    bool active{true};
};

/// A parsed list file.
struct PluginList {
    std::vector<ListedPlugin> plugins;

    /// Whether any line had the `*` active marker. `plugins.txt` uses it,
    /// `loadorder.txt` does not (every line counts). Needed to tell them apart.
    bool marks_active{};
};

/// Parse a list file. Handles a UTF-8 BOM, CRLF, blank lines, `#` comments and
/// surrounding whitespace.
[[nodiscard]] PluginList parse_plugin_list(std::string_view text);

[[nodiscard]] io::ParseResult<PluginList> read_plugin_list(const std::filesystem::path& path);

/// One plugin, placed.
struct LoadOrderEntry {
    std::string name; ///< Filename as spelled in the list.
    std::filesystem::path path;
    bool active{true};
    bool is_master{};
    bool is_light{};
    bool is_localized{};
    float header_version{};
    std::vector<std::string> masters;

    /// Position of each master in the order, or `npos` if missing. Parallel to
    /// `masters`.
    std::vector<std::size_t> master_slots;

    /// 0x00..0xFD for normal plugins, 0x000..0xFFF in the light space.
    std::uint32_t index{};

    /// High bits of a FormID from this plugin after remapping.
    [[nodiscard]] std::uint32_t form_prefix() const noexcept {
        return is_light ? (k_light_prefix | (index << 12)) : (index << 24);
    }
    [[nodiscard]] std::uint32_t object_mask() const noexcept {
        return is_light ? k_light_object_mask : k_normal_object_mask;
    }
};

/// A load-order issue to report; never a reason to stop.
struct LoadOrderProblem {
    enum class Kind : std::uint8_t {
        not_found,             ///< Listed, but not in the data folder.
        unreadable,            ///< Present, but the TES4 header does not parse.
        duplicate,             ///< Listed twice; the later entry is dropped.
        missing_master,        ///< A declared master is not in the order.
        master_after_dependent,///< A master loads after something that needs it.
        too_many_normal,       ///< More than 254 normal plugins.
        too_many_light,        ///< More than 4096 light plugins.
    };

    Kind kind{};
    std::string plugin;
    std::string detail;

    [[nodiscard]] std::string to_string() const;
};

[[nodiscard]] std::string_view to_string(LoadOrderProblem::Kind kind) noexcept;

/// Load order build options. At namespace scope because a nested class with
/// default member initializers cannot be used as a `= {}` default argument.
struct LoadOrderOptions {
    /// Skip unmarked plugins in a `plugins.txt`. Ignored for lists without
    /// markers.
    bool active_only = true;

    /// Prepend implicit masters that exist and are not listed.
    bool add_implicit_masters = true;

    /// Plugins the game loads right after the implicit masters whatever the
    /// list says, in this order: Skyrim SE's Creation Club list (`Skyrim.ccc`
    /// next to the executable; install::creation_club_plugins). Hoisted like
    /// the masters; missing files are skipped. LOOT's libloadorder treats them
    /// the same way ("early loading plugins"). Without them, a plugins.txt
    /// that lists no Creation Club content drops what the game loads anyway.
    std::vector<std::string> always_loaded;
};

/// A resolved load order: plugins, order and indices.
class LoadOrder {
public:
    /// Build from a list. Names match the data folder case-insensitively.
    [[nodiscard]] static LoadOrder build(const std::filesystem::path& data_dir,
                                         const PluginList& list, const LoadOrderOptions& options = {});

    /// Build from a list, finding plugins in several folders. A name present
    /// in more than one folder is taken from the last: Mod Organizer's mods
    /// overlay the game's Data folder in priority order, and the cleaned
    /// masters a list ships must replace the game's.
    [[nodiscard]] static LoadOrder build(std::span<const std::filesystem::path> plugin_dirs,
                                         const PluginList& list,
                                         const LoadOrderOptions& options = {});

    /// Build from the data folder alone, as the game does without a
    /// `plugins.txt`: implicit masters, then by modification time, then name.
    /// Only as good as the timestamps; prefer the user's list file.
    [[nodiscard]] static io::ParseResult<LoadOrder> from_directory(
        const std::filesystem::path& data_dir, const LoadOrderOptions& options = {});

    [[nodiscard]] const std::vector<LoadOrderEntry>& entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] const std::vector<LoadOrderProblem>& problems() const noexcept {
        return problems_;
    }

    /// Position of `name` (case-insensitive), or nullopt.
    [[nodiscard]] std::optional<std::size_t> find(std::string_view name) const;

    [[nodiscard]] std::size_t normal_count() const noexcept { return normal_count_; }
    [[nodiscard]] std::size_t light_count() const noexcept { return light_count_; }

    /// Map a FormID written inside `plugin` to the global space. Fails if the
    /// high byte is past the master list (vanilla Skyrim.esm has one such GMST)
    /// or the named master is missing.
    [[nodiscard]] io::ParseResult<FormId> resolve(std::size_t plugin, FormId local) const;

    /// The plugin a FormID written inside `plugin` refers to. Same failure
    /// cases as resolve().
    [[nodiscard]] io::ParseResult<std::size_t> owner_of(std::size_t plugin,
                                                        FormId local) const;

private:
    std::vector<LoadOrderEntry> entries_;
    std::vector<LoadOrderProblem> problems_;
    std::size_t normal_count_{};
    std::size_t light_count_{};
};

} // namespace bethconv::record
