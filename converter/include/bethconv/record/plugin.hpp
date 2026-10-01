// SPDX-License-Identifier: GPL-3.0-or-later
//
// Structural walk of an ESM/ESP/ESL plugin: TES4 header, GRUP tree, record and
// field boundaries, without knowing what records mean. Field definitions sit on
// top, so an unknown record type costs one record, not the walk.
//
// Source: <https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format>.
#pragma once

#include "bethconv/io/mapped_file.hpp"
#include "bethconv/record/headers.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace bethconv::record {

/// One master of a plugin, in order. A raw FormID's mod byte indexes this list.
struct MasterEntry {
    std::string name;
    std::uint64_t size{}; ///< The DATA field; 0 in practice.
};

/// The TES4 record describing the plugin.
struct PluginHeader {
    float version{};              ///< HEDR: 0.94 for Skyrim LE, 1.70/1.71 for SE.
    std::int32_t record_count{};  ///< HEDR: records excluding TES4 itself.
    std::uint32_t next_object_id{};
    std::string author;           ///< CNAM
    std::string description;      ///< SNAM
    std::vector<MasterEntry> masters;
    std::vector<FormId> overrides;   ///< ONAM: forms this plugin overrides.
    std::uint32_t internal_version{}; ///< INTV
    std::uint32_t unknown_incc{};     ///< INCC

    std::uint32_t flags{};

    [[nodiscard]] bool is_master() const noexcept {
        return has_flag(flags, RecordFlag::master);
    }
    /// ESL / light master: FormIds live in the 0xFE compact space.
    [[nodiscard]] bool is_light() const noexcept {
        return has_flag(flags, RecordFlag::light_master);
    }
    /// Strings are externalized into STRINGS/DLSTRINGS/ILSTRINGS files.
    [[nodiscard]] bool is_localized() const noexcept {
        return has_flag(flags, RecordFlag::localized);
    }
};

/// Position of a record in the GRUP tree, so a sink knows e.g. that a REFR is a
/// persistent child of cell X in worldspace Y without its own stack.
struct GroupContext {
    GroupHeader header;
};

struct RecordContext {
    RecordHeader header;
    /// Innermost-last. Empty for a record at file top level (only TES4).
    std::span<const GroupContext> groups;

    /// Record type of the enclosing top-level GRUP (e.g. "CELL"); null FourCC if
    /// there is none.
    [[nodiscard]] FourCC top_group_type() const noexcept;
};

/// Callbacks for a plugin walk. Defaults ignore everything.
class RecordSink {
public:
    virtual ~RecordSink() = default;

    /// `data` reads the record's payload, already inflated, and cannot read
    /// past the record.
    virtual void on_record(const RecordContext& ctx, io::SpanReader& data) = 0;

    virtual void on_group_enter(const GroupContext&) {}
    virtual void on_group_exit(const GroupContext&) {}

    /// A non-fatal parse failure. Return false to stop; the default continues.
    virtual bool on_error(const io::ParseError&) { return true; }
};

/// Scan statistics, used by `bethconv records --stats` and the corpus harness.
struct ScanStats {
    std::uint64_t records{};
    std::uint64_t groups{};
    std::uint64_t compressed_records{};
    std::uint64_t deleted_records{};
    std::uint64_t bytes_scanned{};
    std::uint64_t bytes_inflated{};
    std::uint64_t errors{};
};

/// A mapped plugin file, its TES4 header, and the ability to walk it.
class Plugin {
public:
    [[nodiscard]] static io::ParseResult<Plugin> open(const std::filesystem::path& path);

    [[nodiscard]] const PluginHeader& header() const noexcept { return header_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::size_t size() const noexcept { return file_.size(); }

    /// Walks every group and record after the TES4 header.
    ///
    /// Errors go to the sink. A malformed record aborts its enclosing group
    /// (there is no way to resync inside it) and the walk continues with the
    /// parent.
    [[nodiscard]] ScanStats scan(RecordSink& sink) const;

private:
    io::MappedFile file_;
    PluginHeader header_;
    std::string name_;
    std::size_t body_offset_{}; ///< First byte after the TES4 record.
};

/// Parses a TES4 record payload into a PluginHeader.
[[nodiscard]] io::ParseResult<PluginHeader> parse_plugin_header(io::SpanReader& data,
                                                                std::uint32_t flags);

} // namespace bethconv::record
