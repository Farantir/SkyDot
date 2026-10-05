// SPDX-License-Identifier: GPL-3.0-or-later
//
// What several subcommands share: mounting, the load order `merge` builds,
// lists of virtual paths, the output-target check and the exit status.
#pragma once

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/io/parse_error.hpp"
#include "bethconv/record/load_order.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bethconv::cli {

/// Help text of `--allow-slow-target`, which every command that writes many
/// files takes.
inline constexpr const char* k_allow_slow_help =
    "Write many small files even to a FUSE filesystem or a spinning disk";

/// What `main` returns. A CLI11 callback cannot return a value, so the
/// command it runs leaves its exit status here.
void set_exit_status(int status) noexcept;
[[nodiscard]] int exit_status() noexcept;

/// Mount every archive and loose dir given, in order; later mounts win ties.
int mount_all(archive::ArchiveSet& set, const std::vector<std::filesystem::path>& paths);

/// Mount a Data folder: every archive (alphabetically, not in load order), then
/// loose files, which win as in the game. The command reports how many
/// `strings/` paths have more than one provider rather than assuming none.
std::size_t mount_data_folder(archive::ArchiveSet& set, const std::filesystem::path& data_dir);

/// Build a load order from `list_file`, or from the Data folder when it is empty.
io::ParseResult<record::LoadOrder> build_order(const std::filesystem::path& data_dir,
                                               const std::filesystem::path& list_file);

/// Read one virtual path per line, ignoring blanks and `#` comments. Lets many
/// files share one mount (mounting SE's texture archives takes ~0.8 s).
std::vector<std::string> read_vpath_list(const std::filesystem::path& path, bool& ok);

/// A FormID or string index typed on the command line. Base 0 reads `0x`
/// hex, a leading `0` as octal and anything else as decimal, as `strtoul`
/// does; base 16 takes hex with or without `0x`. Nothing but the number may
/// be there and it must fit 32 bits, otherwise `nullopt`.
[[nodiscard]] std::optional<std::uint32_t> parse_u32(std::string_view text, int base = 0);

/// parse_u32 over every value of a repeatable option, before any work starts.
/// The first bad value is printed as an error naming `option`; the caller
/// then returns 2, the status of a usage error.
[[nodiscard]] std::optional<std::vector<std::uint32_t>> parse_ids(
    const std::vector<std::string>& texts, const char* option);

/// check_target (front_end.hpp), printed: refusals as errors, the rest as
/// warnings.
bool output_target_ok(const std::filesystem::path& out, bool many_files, bool allow);

} // namespace bethconv::cli
