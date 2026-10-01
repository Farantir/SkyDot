// SPDX-License-Identifier: GPL-3.0-or-later
//
// .STRINGS/.DLSTRINGS/.ILSTRINGS tables: where localized plugins keep their
// text. Such plugins store 4-byte indices instead of names.
//
// An index only has meaning together with its plugin, so strings are resolved
// at or before the merge, which discards that provenance.
//
// Measured against the vanilla English tables in `Skyrim - Interface.bsa`
// (30,301 + 2,686 + 34,427 entries). Source:
// <https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format> ("Localized strings").
#pragma once

#include "bethconv/io/span_reader.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bethconv::record {

/// Which table a string field reads from. Depends on the field: FULL and short
/// labels are `plain`, long book/quest/perk texts `description`, dialogue
/// responses `dialogue`. The id sets of the three vanilla tables are disjoint,
/// but the kind is still never guessed.
enum class StringKind : std::uint8_t {
    plain,       ///< .STRINGS -- FULL and other short labels.
    description, ///< .DLSTRINGS -- DESC, CNAM: long text.
    dialogue,    ///< .ILSTRINGS -- INFO response text.
};

/// "STRINGS", "DLSTRINGS", "ILSTRINGS".
[[nodiscard]] std::string_view to_string(StringKind kind) noexcept;

/// ".strings", ".dlstrings", ".ilstrings" (lowercase, as in a vpath).
[[nodiscard]] std::string_view extension_of(StringKind kind) noexcept;

inline constexpr std::array<StringKind, 3> k_string_kinds{
    StringKind::plain, StringKind::description, StringKind::dialogue};

/// Default language. Bethesda uses names, not locale codes.
inline constexpr std::string_view k_default_language = "english";

/// Virtual path of a plugin's table: `strings/<plugin stem, lowercased>_<language>.<ext>`,
/// e.g. `Skyrim.esm` -> `strings/skyrim_english.strings`.
///
/// Looked up through the archive set, since SE keeps them in
/// `Skyrim - Interface.bsa` and LE ships loose files in `Data/Strings/`; loose
/// translation overrides then win as in the game.
[[nodiscard]] std::string string_table_path(std::string_view plugin_name,
                                            std::string_view language, StringKind kind);

/// Statistics for one table.
struct StringTableStats {
    std::uint64_t entries{};          ///< Directory entries, == distinct ids.
    std::uint64_t distinct_strings{}; ///< Distinct data offsets.
    std::uint64_t text_bytes{};       ///< Decoded text, terminators excluded.
    /// Entries that were not valid UTF-8 and were repaired. Zero across 161,443
    /// vanilla entries (English, German, French, Russian); mod tables may
    /// differ.
    std::uint64_t repaired{};
};

/// One table: string id -> text.
///
/// Several ids can share one stored string (8,601 of 30,301 in
/// `skyrim_english.strings`), so texts are stored once and ids index into
/// them. The directory is not sorted by offset.
class StringTable {
public:
    StringTable() = default;

    /// Parse a whole table. `kind` decides the entry framing: NUL-terminated for
    /// `.strings`, a uint32 length including the NUL for the other two.
    [[nodiscard]] static io::ParseResult<StringTable> parse(std::span<const std::byte> bytes,
                                                            std::string_view origin,
                                                            StringKind kind);

    /// Null if the id is not in the table.
    [[nodiscard]] const std::string* find(std::uint32_t id) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
    [[nodiscard]] bool empty() const noexcept { return index_.empty(); }
    [[nodiscard]] const StringTableStats& stats() const noexcept { return stats_; }
    [[nodiscard]] StringKind kind() const noexcept { return kind_; }

private:
    std::unordered_map<std::uint32_t, std::uint32_t> index_; ///< id -> texts_ slot.
    std::vector<std::string> texts_;
    StringTableStats stats_;
    StringKind kind_{};
};

/// The three tables belonging to one plugin, in one language.
class StringSource {
public:
    void set(StringKind kind, StringTable table);

    /// Resolve an index. Null if the table is missing or lacks the id.
    [[nodiscard]] const std::string* find(std::uint32_t id, StringKind kind) const noexcept;

    [[nodiscard]] const StringTable& table(StringKind kind) const noexcept;

    [[nodiscard]] bool empty() const noexcept;
    /// Entries across all three tables.
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::uint64_t repaired() const noexcept;

private:
    std::array<StringTable, 3> tables_;
};

/// Supplies table bytes to `load_string_source` without record/ depending on
/// the archive layer. A lambda over an ArchiveSet in practice, a map in tests.
using StringFetch =
    std::function<std::optional<std::vector<std::byte>>(std::string_view vpath)>;

/// Load all three tables for one plugin.
///
/// Missing tables are fine (many plugins ship only `.strings`). A table that
/// fails to parse is added to `problems`; the others still load.
[[nodiscard]] StringSource load_string_source(const StringFetch& fetch,
                                              std::string_view plugin_name,
                                              std::string_view language,
                                              std::vector<io::ParseError>* problems = nullptr);

} // namespace bethconv::record
