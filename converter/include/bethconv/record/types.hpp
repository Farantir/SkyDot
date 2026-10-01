// SPDX-License-Identifier: GPL-3.0-or-later
//
// Core value types of the ESM4 record format.
//
// Source: <https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format>, checked
// against xEdit's record definitions.
#pragma once

#include "bethconv/io/span_reader.hpp"

#include <cstdint>
#include <string>

namespace bethconv::record {

using io::FourCC;

/// A raw, plugin-local FormID: the high byte indexes the plugin's master list,
/// the rest is the object index. Only meaningful with its plugin; the merge
/// remaps all of them into one global space.
struct FormId {
    std::uint32_t value{};

    friend constexpr bool operator==(FormId, FormId) noexcept = default;
    friend constexpr auto operator<=>(FormId, FormId) noexcept = default;

    [[nodiscard]] constexpr bool is_null() const noexcept { return value == 0; }

    /// Mod index of a normal (non-light) FormID: the top byte.
    [[nodiscard]] constexpr std::uint8_t mod_index() const noexcept {
        return static_cast<std::uint8_t>(value >> 24);
    }

    /// Light-plugin (ESL) FormID: 0xFE marker, 12-bit plugin index, 12-bit
    /// object index. Source: UESP, ESL section.
    [[nodiscard]] constexpr bool is_light() const noexcept { return mod_index() == 0xFE; }

    [[nodiscard]] constexpr std::uint16_t light_index() const noexcept {
        return static_cast<std::uint16_t>((value >> 12) & 0xFFF);
    }

    [[nodiscard]] std::string to_string() const;
};

/// Record header flags. Only flags we act on are named; all flags are kept
/// as is.
enum class RecordFlag : std::uint32_t {
    master = 0x00000001,           ///< On TES4 only: this plugin is an ESM.
    deleted = 0x00000020,          ///< Record deleted by an override.
    constant = 0x00000040,         ///< aka "hidden from local map" on REFR.
    localized = 0x00000080,        ///< On TES4 only: strings live in .STRINGS files.
    light_master = 0x00000200,     ///< On TES4 only: ESL. Source: UESP, ESL section.
    persistent = 0x00000400,       ///< REFR persistent reference / "off limits".
    initially_disabled = 0x00000800,
    ignored = 0x00001000,
    visible_when_distant = 0x00008000,
    compressed = 0x00040000,       ///< Record data is zlib-compressed.
    cant_wait = 0x00080000,
};

[[nodiscard]] constexpr bool has_flag(std::uint32_t flags, RecordFlag f) noexcept {
    return (flags & static_cast<std::uint32_t>(f)) != 0;
}

/// GRUP group types.
///
/// Value 10 is "Cell Visible Distant Children" in Oblivion and "Quest Children"
/// in Skyrim; the names here are Skyrim's.
enum class GroupType : std::int32_t {
    top = 0,                      ///< label = the record type contained
    world_children = 1,           ///< label = parent WRLD FormId
    interior_cell_block = 2,      ///< label = block number
    interior_cell_sub_block = 3,  ///< label = sub-block number
    exterior_cell_block = 4,      ///< label = packed grid Y|X (int16 each)
    exterior_cell_sub_block = 5,  ///< label = packed grid Y|X
    cell_children = 6,            ///< label = parent CELL FormId
    topic_children = 7,           ///< label = parent DIAL FormId
    cell_persistent_children = 8, ///< label = parent CELL FormId
    cell_temporary_children = 9,  ///< label = parent CELL FormId
    quest_children = 10,          ///< label = parent QUST FormId (TES5)
};

[[nodiscard]] std::string_view to_string(GroupType type) noexcept;

/// A group's label, whose meaning depends on the group type.
struct GroupLabel {
    std::uint32_t raw{};

    [[nodiscard]] FourCC as_type() const noexcept { return FourCC{raw}; }
    [[nodiscard]] FormId as_form() const noexcept { return FormId{raw}; }

    /// Exterior cell block labels pack two int16 grid coordinates: Y in the high
    /// half, X in the low half. Source: UESP, GRUP section.
    [[nodiscard]] std::int16_t grid_x() const noexcept {
        return static_cast<std::int16_t>(raw & 0xFFFF);
    }
    [[nodiscard]] std::int16_t grid_y() const noexcept {
        return static_cast<std::int16_t>(raw >> 16);
    }

    /// Rendered according to `type`, for diagnostics.
    [[nodiscard]] std::string to_string(GroupType type) const;
};

} // namespace bethconv::record
