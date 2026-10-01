// SPDX-License-Identifier: GPL-3.0-or-later
//
// The three fixed headers of the ESM4 format: record, group and field. Source:
// <https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format>.
#pragma once

#include "bethconv/record/types.hpp"

#include <cstdint>

namespace bethconv::record {

/// Record and group headers are 24 bytes in Skyrim. (Oblivion records use 20;
/// named so that can be supported later.)
inline constexpr std::size_t k_record_header_size = 24;
inline constexpr std::size_t k_group_header_size = 24;

/// Field (subrecord) headers are 6 bytes: a 4-byte type and a uint16 size.
inline constexpr std::size_t k_field_header_size = 6;

/// Upper bound on one record's payload. Vanilla records are well under 1 MB;
/// this stops a corrupt size from causing a huge allocation.
inline constexpr std::uint32_t k_max_record_data_size = 64u * 1024u * 1024u;

struct RecordHeader {
    FourCC type;
    std::uint32_t data_size{};  ///< Payload only; excludes this header.
    std::uint32_t flags{};
    FormId form_id;
    std::uint32_t revision{};
    std::uint16_t version{};
    std::uint16_t unknown{};

    [[nodiscard]] bool is_compressed() const noexcept {
        return has_flag(flags, RecordFlag::compressed);
    }
    [[nodiscard]] bool is_deleted() const noexcept {
        return has_flag(flags, RecordFlag::deleted);
    }
};

struct GroupHeader {
    /// Includes the 24-byte group header, unlike RecordHeader::data_size.
    std::uint32_t group_size{};
    GroupLabel label;
    GroupType group_type{};
    std::uint16_t stamp{};
    std::uint16_t unknown{};
    std::uint16_t version{};
    std::uint16_t unknown2{};

    /// Payload size without the header; an error if the group is smaller than
    /// its own header.
    [[nodiscard]] io::ParseResult<std::uint32_t> payload_size(
        const io::SpanReader& at) const;
};

struct FieldHeader {
    FourCC type;
    std::uint32_t data_size{}; ///< uint16 on disk; widened by an XXXX prefix.
};

    /// Reads a 24-byte record header. The 4-byte type must not have been read yet.
[[nodiscard]] io::ParseResult<RecordHeader> read_record_header(io::SpanReader& reader);

/// Reads the 20 bytes of a group header that follow the "GRUP" tag.
[[nodiscard]] io::ParseResult<GroupHeader> read_group_header_body(io::SpanReader& reader);

/// Reads a field header, handling the XXXX escape: an "XXXX" field holding a
/// uint32 gives the size of the next field, whose own uint16 size is 0. Used for
/// fields over 64 KiB (large NAVM/LAND). Source: UESP, Fields section.
[[nodiscard]] io::ParseResult<FieldHeader> read_field_header(io::SpanReader& reader);

} // namespace bethconv::record
