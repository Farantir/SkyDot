// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/load_order.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>

namespace bethconv::record {
namespace {

constexpr std::size_t k_npos = std::numeric_limits<std::size_t>::max();

/// Case-insensitive ASCII comparison, as Windows compares plugin names. Not
/// locale-aware, so results do not depend on the machine.
[[nodiscard]] std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

[[nodiscard]] bool is_plugin_extension(const std::filesystem::path& path) {
    const auto ext = ascii_lower(path.extension().string());
    return ext == ".esm" || ext == ".esp" || ext == ".esl";
}

[[nodiscard]] std::string_view trim(std::string_view text) {
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

/// Plugin files directly in `dir`, keyed by lowercased name. Subdirectories are
/// ignored, as by the game.
[[nodiscard]] std::vector<std::pair<std::string, std::filesystem::path>>
index_directory(const std::filesystem::path& dir) {
    std::vector<std::pair<std::string, std::filesystem::path>> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec) || !is_plugin_extension(it->path())) {
            continue;
        }
        out.emplace_back(ascii_lower(it->path().filename().string()), it->path());
    }
    std::ranges::sort(out, {}, &std::pair<std::string, std::filesystem::path>::first);
    return out;
}

/// index_directory over several folders; a later folder replaces an earlier
/// one's file of the same name.
[[nodiscard]] std::vector<std::pair<std::string, std::filesystem::path>>
index_directories(std::span<const std::filesystem::path> dirs) {
    std::vector<std::pair<std::string, std::filesystem::path>> out;
    for (const auto& dir : dirs) {
        for (auto& [key, path] : index_directory(dir)) {
            const auto it = std::ranges::lower_bound(
                out, key, {}, &std::pair<std::string, std::filesystem::path>::first);
            if (it != out.end() && it->first == key) {
                it->second = std::move(path);
            } else {
                out.emplace(it, std::move(key), std::move(path));
            }
        }
    }
    return out;
}

[[nodiscard]] const std::filesystem::path* lookup(
    const std::vector<std::pair<std::string, std::filesystem::path>>& index,
    std::string_view name) {
    const auto key = ascii_lower(name);
    const auto it = std::ranges::lower_bound(index, key, {},
                                             &std::pair<std::string, std::filesystem::path>::first);
    if (it == index.end() || it->first != key) {
        return nullptr;
    }
    return &it->second;
}

} // namespace

std::span<const std::string_view> implicit_masters() noexcept {
    // Game order; SkyrimVR.esm loads after the DLC. Missing files are skipped,
    // so one list works for LE, SE and VR.
    static constexpr std::array<std::string_view, 6> k_implicit{
        "Skyrim.esm",     "Update.esm",     "Dawnguard.esm",
        "HearthFires.esm", "Dragonborn.esm", "SkyrimVR.esm",
    };
    return k_implicit;
}

std::string_view to_string(LoadOrderProblem::Kind kind) noexcept {
    switch (kind) {
    case LoadOrderProblem::Kind::not_found:              return "not-found";
    case LoadOrderProblem::Kind::unreadable:             return "unreadable";
    case LoadOrderProblem::Kind::duplicate:              return "duplicate";
    case LoadOrderProblem::Kind::missing_master:         return "missing-master";
    case LoadOrderProblem::Kind::master_after_dependent: return "master-after-dependent";
    case LoadOrderProblem::Kind::too_many_normal:        return "too-many-normal";
    case LoadOrderProblem::Kind::too_many_light:         return "too-many-light";
    }
    return "?";
}

std::string LoadOrderProblem::to_string() const {
    return plugin + ": " + std::string(record::to_string(kind)) +
           (detail.empty() ? std::string{} : ": " + detail);
}

PluginList parse_plugin_list(std::string_view text) {
    PluginList list;

    // Strip a UTF-8 BOM, which would otherwise become part of the first name.
    constexpr std::string_view bom = "\xEF\xBB\xBF";
    if (text.starts_with(bom)) {
        text.remove_prefix(bom.size());
    }

    while (!text.empty()) {
        const auto newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{}
                                                 : text.substr(newline + 1);
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        bool active = false;
        if (line.front() == '*') {
            active = true;
            list.marks_active = true;
            line = trim(line.substr(1));
            if (line.empty()) {
                continue;
            }
        }
        list.plugins.push_back(ListedPlugin{.name = std::string(line), .active = active});
    }

    // No markers means loadorder.txt: every line is active.
    if (!list.marks_active) {
        for (auto& plugin : list.plugins) {
            plugin.active = true;
        }
    }
    return list;
}

io::ParseResult<PluginList> read_plugin_list(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        return std::unexpected(io::ParseError{.origin = path.filename().string(),
                                              .offset = 0,
                                              .kind = io::ErrorKind::bad_value,
                                              .detail = "cannot open plugin list"});
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    return parse_plugin_list(text);
}

LoadOrder LoadOrder::build(const std::filesystem::path& data_dir, const PluginList& list,
                           const LoadOrderOptions& options) {
    return build(std::span(&data_dir, 1), list, options);
}

LoadOrder LoadOrder::build(std::span<const std::filesystem::path> plugin_dirs,
                           const PluginList& list, const LoadOrderOptions& options) {
    LoadOrder order;
    const auto index = index_directories(plugin_dirs);

    // ---- the name sequence, before anything is opened ---------------------

    std::vector<ListedPlugin> sequence;
    std::vector<std::string> seen;
    const auto already = [&](std::string_view name) {
        return std::ranges::find(seen, ascii_lower(name)) != seen.end();
    };
    const auto push = [&](const ListedPlugin& plugin) {
        seen.push_back(ascii_lower(plugin.name));
        sequence.push_back(plugin);
    };

    // The only reordering: base game and DLC go first, as the game always loads
    // them. MO2 omits them from plugins.txt and lists them first in
    // loadorder.txt, so this is a no-op or a fix. All of them are hoisted, not
    // just unlisted ones, so Dawnguard can never end up before Skyrim.esm.
    if (options.add_implicit_masters) {
        for (const auto name : implicit_masters()) {
            if (lookup(index, name) != nullptr) {
                // Always active; the game gives no choice.
                push(ListedPlugin{.name = std::string(name), .active = true});
            }
        }
    }
    for (const auto& name : options.always_loaded) {
        if (lookup(index, name) != nullptr && !already(name)) {
            push(ListedPlugin{.name = name, .active = true});
        }
    }

    for (const auto& plugin : list.plugins) {
        if (list.marks_active && options.active_only && !plugin.active) {
            continue;
        }
        if (already(plugin.name)) {
            const auto same = [&](std::string_view m) {
                return ascii_lower(m) == ascii_lower(plugin.name);
            };
            const bool implicit = std::ranges::any_of(implicit_masters(), same) ||
                                  std::ranges::any_of(options.always_loaded, same);
            if (!implicit) {
                order.problems_.push_back(LoadOrderProblem{
                    .kind = LoadOrderProblem::Kind::duplicate,
                    .plugin = plugin.name,
                    .detail = "listed more than once; the later mention is dropped"});
            }
            continue;
        }
        push(plugin);
    }

    // ---- open each one and read its header --------------------------------

    for (const auto& listed : sequence) {
        const auto* path = lookup(index, listed.name);
        if (path == nullptr) {
            order.problems_.push_back(
                LoadOrderProblem{.kind = LoadOrderProblem::Kind::not_found,
                                 .plugin = listed.name,
                                 .detail = "no such plugin in the data folder"});
            continue;
        }

        auto plugin = Plugin::open(*path);
        if (!plugin) {
            order.problems_.push_back(
                LoadOrderProblem{.kind = LoadOrderProblem::Kind::unreadable,
                                 .plugin = listed.name,
                                 .detail = plugin.error().to_string()});
            continue;
        }

        const auto& header = plugin->header();
        LoadOrderEntry entry;
        entry.name = listed.name;
        entry.path = *path;
        entry.active = listed.active;
        // From the header, never the extension (234 of 612 modded plugins are
        // light-flagged, 3 named .esl).
        entry.is_master = header.is_master();
        entry.is_light = header.is_light();
        entry.is_localized = header.is_localized();
        entry.header_version = header.version;
        entry.masters.reserve(header.masters.size());
        for (const auto& master : header.masters) {
            entry.masters.push_back(master.name);
        }
        order.entries_.push_back(std::move(entry));
    }

    // ---- indices ----------------------------------------------------------

    std::uint32_t next_normal = 0;
    std::uint32_t next_light = 0;
    for (auto& entry : order.entries_) {
        if (entry.is_light) {
            if (next_light > k_max_light_index) {
                order.problems_.push_back(LoadOrderProblem{
                    .kind = LoadOrderProblem::Kind::too_many_light,
                    .plugin = entry.name,
                    .detail = "the 0xFE space holds " + std::to_string(k_max_light_index + 1) +
                              " plugins"});
                continue;
            }
            entry.index = next_light++;
            ++order.light_count_;
        } else {
            if (next_normal > k_max_normal_index) {
                order.problems_.push_back(LoadOrderProblem{
                    .kind = LoadOrderProblem::Kind::too_many_normal,
                    .plugin = entry.name,
                    .detail = "the normal space ends at 0xFD"});
                continue;
            }
            entry.index = next_normal++;
            ++order.normal_count_;
        }
    }

    // ---- master resolution ------------------------------------------------

    for (std::size_t i = 0; i < order.entries_.size(); ++i) {
        auto& entry = order.entries_[i];
        entry.master_slots.reserve(entry.masters.size());
        for (const auto& master : entry.masters) {
            const auto slot = order.find(master);
            if (!slot) {
                entry.master_slots.push_back(k_npos);
                order.problems_.push_back(
                    LoadOrderProblem{.kind = LoadOrderProblem::Kind::missing_master,
                                     .plugin = entry.name,
                                     .detail = master + " is not in the load order"});
                continue;
            }
            if (*slot > i) {
                // Reported, not fixed: reordering breaks real lists where ESMs
                // depend on ESPs (DynDOLOD.esm).
                order.problems_.push_back(LoadOrderProblem{
                    .kind = LoadOrderProblem::Kind::master_after_dependent,
                    .plugin = entry.name,
                    .detail = master + " loads at position " + std::to_string(*slot) +
                              ", after this plugin at " + std::to_string(i)});
            }
            entry.master_slots.push_back(*slot);
        }
    }

    return order;
}

io::ParseResult<LoadOrder> LoadOrder::from_directory(const std::filesystem::path& data_dir,
                                                     const LoadOrderOptions& options) {
    std::error_code ec;
    if (!std::filesystem::is_directory(data_dir, ec)) {
        return std::unexpected(io::ParseError{.origin = data_dir.filename().string(),
                                              .offset = 0,
                                              .kind = io::ErrorKind::bad_value,
                                              .detail = "not a directory"});
    }

    // build() never sorts a list. Without a list, fall back to the game's own
    // rule: implicit masters, ESM-flagged plugins, the rest, each oldest first.
    // Unreliable, since copies lose timestamps, but better than alphabetical.
    struct Candidate {
        bool master{};
        std::filesystem::file_time_type modified;
        std::string name;
    };
    std::vector<Candidate> candidates;
    for (const auto& [key, path] : index_directory(data_dir)) {
        if (options.add_implicit_masters) {
            const bool implicit = std::ranges::any_of(
                implicit_masters(), [&](std::string_view m) { return ascii_lower(m) == key; });
            if (implicit) {
                continue; // build() adds these first, in game order.
            }
        }
        auto modified = std::filesystem::last_write_time(path, ec);
        if (ec) {
            modified = std::filesystem::file_time_type::min();
            ec.clear();
        }
        bool master = false;
        if (auto plugin = Plugin::open(path)) {
            master = plugin->header().is_master();
        }
        candidates.push_back(Candidate{.master = master,
                                       .modified = modified,
                                       .name = path.filename().string()});
    }
    // Name as tiebreak, so equal timestamps still give a deterministic order.
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
        if (a.master != b.master) {
            return a.master;
        }
        return a.modified != b.modified ? a.modified < b.modified
                                        : ascii_lower(a.name) < ascii_lower(b.name);
    });

    PluginList list;
    for (auto& candidate : candidates) {
        list.plugins.push_back(ListedPlugin{.name = std::move(candidate.name), .active = true});
    }
    return build(data_dir, list, options);
}

std::optional<std::size_t> LoadOrder::find(std::string_view name) const {
    const auto key = ascii_lower(name);
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (ascii_lower(entries_[i].name) == key) {
            return i;
        }
    }
    return std::nullopt;
}

io::ParseResult<std::size_t> LoadOrder::owner_of(std::size_t plugin, FormId local) const {
    if (plugin >= entries_.size()) {
        return std::unexpected(io::ParseError{.origin = "<load order>",
                                              .offset = 0,
                                              .kind = io::ErrorKind::out_of_range,
                                              .detail = "plugin " + std::to_string(plugin) +
                                                        " is not in this order"});
    }
    const auto& entry = entries_[plugin];
    const auto fail = [&](std::string detail) {
        return std::unexpected(io::ParseError{.origin = entry.name,
                                              .offset = 0,
                                              .kind = io::ErrorKind::bad_value,
                                              .detail = std::move(detail)});
    };

    const std::size_t slot = local.mod_index();
    if (slot == entry.masters.size()) {
        return plugin;
    }
    if (slot > entry.masters.size()) {
        // Vanilla Skyrim.esm has one of these (GMST 0x0123C00E, index 1 with no
        // masters); dirty mods have more. Reported, not fatal.
        return fail(local.to_string() + " names master index " + std::to_string(slot) +
                    ", past the " + std::to_string(entry.masters.size()) +
                    " this plugin declares");
    }
    if (entry.master_slots[slot] == k_npos) {
        return fail(local.to_string() + " names master " + entry.masters[slot] +
                    ", which is not in the load order");
    }
    return entry.master_slots[slot];
}

io::ParseResult<FormId> LoadOrder::resolve(std::size_t plugin, FormId local) const {
    const auto owner = owner_of(plugin, local);
    if (!owner) {
        return std::unexpected(owner.error());
    }
    const auto& target = entries_[*owner];
    const std::uint32_t object = local.value & k_normal_object_mask;

    if (target.is_light && object > k_light_object_mask) {
        return std::unexpected(io::ParseError{
            .origin = entries_[plugin].name,
            .offset = 0,
            .kind = io::ErrorKind::bad_value,
            .detail = local.to_string() + " has object index 0x" + FormId{object}.to_string() +
                      ", which does not fit the 12 bits a light plugin gets"});
    }
    return FormId{target.form_prefix() | (object & target.object_mask())};
}

} // namespace bethconv::record
