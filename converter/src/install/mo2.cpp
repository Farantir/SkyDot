// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/install/mo2.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

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

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }
    return text;
}

/// Calls `fn(line)` for every line without its line ending.
template <typename Fn>
void for_each_line(std::string_view text, Fn&& fn) {
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        fn(line);
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + 1);
    }
}

[[nodiscard]] bool is_dir(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

[[nodiscard]] int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] bool is_windows_absolute(std::string_view path) noexcept {
    return path.size() >= 2 && path[1] == ':' &&
           ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'));
}

/// A path from the INI, made usable here: %BASE_DIR% expanded, backslashes
/// turned into this system's separator, relative paths taken from `base`.
[[nodiscard]] std::filesystem::path local_path(std::string written,
                                               const std::filesystem::path& base) {
    constexpr std::string_view k_base = "%BASE_DIR%";
    if (const auto at = written.find(k_base); at != std::string::npos) {
        written.replace(at, k_base.size(), base.generic_string());
    }
#ifndef _WIN32
    std::ranges::replace(written, '\\', '/');
#endif
    std::filesystem::path path(written);
    if (path.is_relative() && !is_windows_absolute(written)) {
        return base / path;
    }
    return path;
}

/// The folder a setting names if it exists, else `fallback`.
[[nodiscard]] std::filesystem::path existing_or(const std::string& written,
                                                const std::filesystem::path& base,
                                                const std::filesystem::path& fallback) {
    if (!written.empty()) {
        auto path = local_path(written, base);
        if (is_dir(path)) {
            return path;
        }
    }
    return fallback;
}

/// The last component of a path written with either separator.
[[nodiscard]] std::string_view last_component(std::string_view path) noexcept {
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) {
        path.remove_suffix(1);
    }
    const auto at = path.find_last_of("/\\");
    return at == std::string_view::npos ? path : path.substr(at + 1);
}

} // namespace

std::string decode_ini_value(std::string_view raw) {
    auto text = trim(raw);
    if (text.starts_with("@ByteArray(") && text.ends_with(')')) {
        text = text.substr(11, text.size() - 12);
    } else if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
    }
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c != '\\' || i + 1 >= text.size()) {
            out.push_back(c);
            continue;
        }
        const char e = text[++i];
        switch (e) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case '0': out.push_back('\0'); break;
        case 'x': {
            int value = 0;
            int digits = 0;
            while (digits < 2 && i + 1 < text.size() && hex_digit(text[i + 1]) >= 0) {
                value = value * 16 + hex_digit(text[++i]);
                ++digits;
            }
            out.push_back(static_cast<char>(value));
            break;
        }
        default: out.push_back(e); break;  // \\ and \" and the rest
        }
    }
    return out;
}

Edition edition_from_game_name(std::string_view name) noexcept {
    const auto lowered = lower(name);
    if (lowered == "skyrim vr" || lowered == "skyrimvr") {
        return Edition::vr;
    }
    if (lowered.starts_with("skyrim special edition") || lowered == "skyrimse" ||
        lowered == "enderal special edition") {
        return Edition::se;
    }
    if (lowered == "skyrim" || lowered == "enderal") {
        return Edition::le;
    }
    return Edition::unknown;
}

std::vector<ModlistEntry> parse_modlist(std::string_view text) {
    std::vector<ModlistEntry> out;
    for_each_line(text, [&](std::string_view line) {
        line = trim(line);
        if (line.size() < 2 || line.front() == '#') {
            return;
        }
        const char state = line.front();
        if (state != '+' && state != '-' && state != '*') {
            return;
        }
        out.push_back(ModlistEntry{.state = state, .name = std::string(line.substr(1))});
    });
    return out;
}

io::ParseResult<Mo2Instance> read_mo2_instance(const std::filesystem::path& dir) {
    const auto ini_path = find_ignoring_case(dir, "ModOrganizer.ini");
    std::optional<std::string> text;
    if (ini_path) {
        text = read_text(*ini_path);
    }
    if (!text) {
        return std::unexpected(io::ParseError{
            .origin = dir.string(),
            .kind = io::ErrorKind::bad_magic,
            .detail = "no ModOrganizer.ini: not a Mod Organizer 2 instance folder"});
    }

    std::string section;
    std::string base_directory;
    std::string mod_directory;
    std::string profiles_directory;
    std::string overwrite_directory;
    Mo2Instance instance;
    instance.dir = dir;
    for_each_line(*text, [&](std::string_view line) {
        line = trim(line);
        if (line.starts_with('[') && line.ends_with(']')) {
            section = lower(line.substr(1, line.size() - 2));
            return;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            return;
        }
        const auto key = lower(trim(line.substr(0, eq)));
        const auto value = line.substr(eq + 1);
        if (section == "general") {
            if (key == "gamename") {
                instance.game_name = decode_ini_value(value);
            } else if (key == "gamepath") {
                instance.game_path_written = decode_ini_value(value);
            } else if (key == "selected_profile") {
                instance.selected_profile = decode_ini_value(value);
            }
        } else if (section == "settings") {
            if (key == "base_directory") {
                base_directory = decode_ini_value(value);
            } else if (key == "mod_directory") {
                mod_directory = decode_ini_value(value);
            } else if (key == "profiles_directory") {
                profiles_directory = decode_ini_value(value);
            } else if (key == "overwrite_directory") {
                overwrite_directory = decode_ini_value(value);
            }
        }
    });

    instance.edition = edition_from_game_name(instance.game_name);

    // A folder setting written on another machine (D:/Modlists/...) does not
    // exist here; the instance's own folder of the default name does.
    const auto base = existing_or(base_directory, dir, dir);
    instance.mods_dir = existing_or(mod_directory, base, base / "mods");
    instance.profiles_dir = existing_or(profiles_directory, base, base / "profiles");
    instance.overwrite_dir = existing_or(overwrite_directory, base, base / "overwrite");

    if (!instance.game_path_written.empty()) {
        auto path = local_path(instance.game_path_written, dir);
        if (is_dir(path)) {
            instance.game_path = std::move(path);
        } else if (const auto name = last_component(instance.game_path_written);
                   !name.empty() && is_dir(dir / name)) {
            instance.game_path = dir / name;
        }
    }

    std::error_code ec;
    for (std::filesystem::directory_iterator it(instance.profiles_dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_directory(ec)) {
            instance.profiles.push_back(it->path().filename().string());
        }
    }
    std::ranges::sort(instance.profiles);
    return instance;
}

io::ParseResult<Mo2Profile> read_mo2_profile(const Mo2Instance& instance, std::string_view name) {
    std::string chosen(name);
    if (chosen.empty()) {
        chosen = instance.selected_profile;
    }
    if (chosen.empty() && instance.profiles.size() == 1) {
        chosen = instance.profiles.front();
    }
    const auto fail = [&](std::string detail) {
        return std::unexpected(io::ParseError{.origin = instance.dir.string(),
                                              .kind = io::ErrorKind::bad_value,
                                              .detail = std::move(detail)});
    };
    if (chosen.empty()) {
        return fail("no profile given and the instance selects none");
    }

    Mo2Profile profile;
    profile.name = chosen;
    profile.dir = instance.profiles_dir / chosen;
    if (!is_dir(profile.dir)) {
        return fail("profile '" + chosen + "' does not exist in " + instance.profiles_dir.string());
    }

    const auto modlist_path = find_ignoring_case(profile.dir, "modlist.txt");
    const auto modlist = modlist_path ? read_text(*modlist_path) : std::nullopt;
    if (!modlist) {
        return fail("profile '" + chosen + "' has no modlist.txt");
    }
    auto entries = parse_modlist(*modlist);
    // The file lists the highest priority first; mounting goes lowest first.
    std::ranges::reverse(entries);
    for (const auto& entry : entries) {
        if (entry.state == '*') {
            ++profile.unmanaged;
            continue;
        }
        if (entry.name.ends_with("_separator")) {
            ++profile.separators;
            continue;
        }
        if (entry.state == '-') {
            ++profile.disabled;
            continue;
        }
        auto mod_dir = instance.mods_dir / entry.name;
        if (is_dir(mod_dir)) {
            profile.mods.push_back(Mo2Mod{.name = entry.name, .dir = std::move(mod_dir)});
        } else {
            profile.missing.push_back(entry.name);
        }
    }

    // Skyrim SE and VR profiles mark active plugins with '*' in plugins.txt.
    // Older games' lists have no marks: plugins.txt then holds the active
    // ones and loadorder.txt the order.
    const auto plugins_path = find_ignoring_case(profile.dir, "plugins.txt");
    const auto loadorder_path = find_ignoring_case(profile.dir, "loadorder.txt");
    std::optional<record::PluginList> plugins;
    if (plugins_path) {
        if (auto list = record::read_plugin_list(*plugins_path)) {
            plugins = std::move(*list);
            profile.plugins_file = *plugins_path;
        }
    }
    if (loadorder_path && (!plugins || !plugins->marks_active)) {
        if (auto order = record::read_plugin_list(*loadorder_path)) {
            if (plugins) {
                std::set<std::string> active;
                for (const auto& plugin : plugins->plugins) {
                    active.insert(lower(plugin.name));
                }
                for (auto& plugin : order->plugins) {
                    plugin.active = active.contains(lower(plugin.name));
                }
                order->marks_active = true;
            } else {
                profile.plugins_file = *loadorder_path;
            }
            plugins = std::move(*order);
        }
    }
    if (plugins) {
        profile.plugins = std::move(*plugins);
    }
    return profile;
}

} // namespace bethconv::install
