// SPDX-License-Identifier: GPL-3.0-or-later
//
// The commands a graphical front end needs, each with `--json` output:
//
//   detect   installs on this machine and their plugins.txt
//   mo2      a Mod Organizer 2 instance, its profiles, one profile's mods
//   target   whether a folder is a good place for a pack
//   info     what a pack holds and how much of its blob is stale
//
// `convert --json` (commands/convert.cpp) reports progress as JSON lines too.
// Every JSON document is one line on stdout; human-readable text goes to
// stderr then. The shapes are documented in docs/cli-json.md.
#pragma once

#include "bethconv/install/game_install.hpp"
#include "bethconv/io/output_target.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace bethconv::cli {

/// Version of the JSON shapes; bumped when a key changes meaning or goes away.
inline constexpr int k_json_version = 1;

enum class TargetLevel : std::uint8_t { ok, warn, refuse };

[[nodiscard]] std::string_view to_string(TargetLevel level) noexcept;

struct TargetVerdict {
    TargetLevel level = TargetLevel::ok;
    std::string reason;  ///< Empty for ok.
    std::optional<io::OutputTarget> target;
};

/// Whether `out` can take what is about to be written. Many small files on a
/// FUSE filesystem or a spinning disk are refused unless `allow`: a loose pack
/// on an SMR disk behind ntfs-3g once hung the whole mount. Large files there
/// only warn. See io/output_target.hpp.
[[nodiscard]] TargetVerdict check_target(const std::filesystem::path& out, bool many_files,
                                         bool allow);

/// Print one JSON document as one line on stdout and flush.
void emit(const nlohmann::ordered_json& doc);

[[nodiscard]] nlohmann::ordered_json install_json(const install::GameInstall& install,
                                                const install::DetectOptions& options);

int cmd_detect(bool json);
int cmd_mo2(const std::filesystem::path& instance, const std::string& profile, bool json);
int cmd_target(const std::filesystem::path& dir, bool json);
int cmd_info(const std::filesystem::path& pack, bool json);

} // namespace bethconv::cli
