// SPDX-License-Identifier: GPL-3.0-or-later
#include "common.hpp"

#include "front_end.hpp"

#include "bethconv/install/mount_plan.hpp"
#include "bethconv/pack/inputs.hpp"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace bethconv::cli {
namespace {

int g_exit_status = 0;

} // namespace

void set_exit_status(int status) noexcept {
    g_exit_status = status;
}

int exit_status() noexcept {
    return g_exit_status;
}

int mount_all(bethconv::archive::ArchiveSet& set,
              const std::vector<std::filesystem::path>& paths) {
    const auto failures = bethconv::pack::mount_sources(set, paths);
    for (const auto& failure : failures) {
        std::fprintf(stderr, "warning: skipping %s\n", failure.c_str());
    }
    return static_cast<int>(failures.size());
}

std::size_t mount_data_folder(bethconv::archive::ArchiveSet& set,
                              const std::filesystem::path& data_dir) {
    const auto plan = bethconv::install::plan_data_folder(data_dir);
    for (const auto& failure : bethconv::install::mount(set, plan)) {
        std::fprintf(stderr, "warning: skipping %s\n", failure.c_str());
    }
    return plan.archives.size() + plan.loose.size();
}

bethconv::io::ParseResult<bethconv::record::LoadOrder> build_order(
    const std::filesystem::path& data_dir, const std::filesystem::path& list_file) {
    if (list_file.empty()) {
        return bethconv::record::LoadOrder::from_directory(data_dir);
    }
    auto list = bethconv::record::read_plugin_list(list_file);
    if (!list) {
        return std::unexpected(std::move(list).error());
    }
    return bethconv::record::LoadOrder::build(data_dir, *list);
}

std::optional<std::uint32_t> parse_u32(std::string_view text, int base) {
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X') &&
        (base == 0 || base == 16)) {
        text.remove_prefix(2);
        base = 16;
    } else if (base == 0) {
        base = text.size() > 1 && text[0] == '0' ? 8 : 10;
    }
    std::uint32_t value = 0;
    const char* const end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value, base);
    if (error != std::errc{} || stop != end) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::vector<std::uint32_t>> parse_ids(const std::vector<std::string>& texts,
                                                    const char* option) {
    std::vector<std::uint32_t> ids;
    ids.reserve(texts.size());
    for (const auto& text : texts) {
        const auto id = parse_u32(text);
        if (!id) {
            std::fprintf(stderr, "error: %s takes numbers (0x1A2B or decimal), got \"%s\"\n",
                         option, text.c_str());
            return std::nullopt;
        }
        ids.push_back(*id);
    }
    return ids;
}

std::vector<std::string> read_vpath_list(const std::filesystem::path& path, bool& ok) {
    std::vector<std::string> out;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "error: cannot read %s\n", path.string().c_str());
        ok = false;
        return out;
    }
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        out.push_back(line);
    }
    ok = true;
    return out;
}

bool output_target_ok(const std::filesystem::path& out, bool many_files, bool allow) {
    const auto verdict = bethconv::cli::check_target(out, many_files, allow);
    if (verdict.level == bethconv::cli::TargetLevel::ok) {
        return true;
    }
    const bool refused = verdict.level == bethconv::cli::TargetLevel::refuse;
    std::fprintf(stderr, "%s: %s\n", refused ? "error" : "warning", verdict.reason.c_str());
    return !refused;
}

} // namespace bethconv::cli
