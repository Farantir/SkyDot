// SPDX-License-Identifier: GPL-3.0-or-later
//
// VMAD: the Papyrus scripts attached to a record and their property values.
//
// Layout: version (int16), object format (int16), script count (uint16), then
// per script a wstring name, a status byte (version >= 4), a property count
// (uint16) and the properties. A property is a wstring name, a type byte, a
// status byte (version >= 4) and a value. Types 1-5 are object, string, int,
// float, bool; 11-15 arrays of those, each prefixed by a uint32 count. Type 0
// has no value (three MCM quests in the FUS list). An
// object is a FormID, an alias index (int16) and two unused bytes, in that
// order for object format 1 and reversed (unused, alias, FormID) for 2.
//
// QUST, INFO, PACK, SCEN and PERK follow the scripts with fragment data.
// QUST's is decoded (`read_quest_fragments`): a byte (2 in vanilla), a
// fragment count, the fragment script's name (empty, not absent, without
// fragments), the fragments (stage uint16, int16, log entry int32, a byte,
// script wstring, function wstring), an alias count and per alias an object,
// a version, an object format and scripts laid out as above. 146 of 2,391 SE
// quests end after the scripts. The other types are not defined, so their remainder is
// leftover.
//
// Source: <https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format/VMAD_Field>,
// checked against every VMAD in the three vanilla installs and the FUS list.
#pragma once

#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bethconv::record {

enum class ScriptPropertyType : std::uint8_t {
    none = 0,
    object = 1,
    string = 2,
    integer = 3,
    floating = 4,
    boolean = 5,
    object_array = 11,
    string_array = 12,
    integer_array = 13,
    floating_array = 14,
    boolean_array = 15,
};

/// True for types 11-15.
[[nodiscard]] constexpr bool is_array(ScriptPropertyType type) noexcept {
    return static_cast<std::uint8_t>(type) >= 11;
}

/// An object property: a form, or with `alias` >= 0 an alias of the quest
/// `form`. FormIDs are plugin-local, as stored.
struct ScriptObject {
    FormId form;
    std::int16_t alias{-1};

    friend constexpr bool operator==(const ScriptObject&, const ScriptObject&) noexcept = default;
};

/// One property. Scalars use one element of the matching vector, arrays any
/// number; bools are stored in `integers` as 0 or 1.
struct ScriptProperty {
    std::string name;
    ScriptPropertyType type{};
    std::uint8_t status{}; ///< 1 edited, 3 removed; 0 before version 4.
    std::vector<ScriptObject> objects;
    std::vector<std::string> strings;
    std::vector<std::int32_t> integers;
    std::vector<float> floats;
};

struct Script {
    std::string name;
    /// 0 local, 1 inherited, 2 removed, 3 inherited and removed.
    std::uint8_t status{};
    std::vector<ScriptProperty> properties;
};

struct ScriptData {
    std::int16_t version{};
    std::int16_t object_format{};
    std::vector<Script> scripts;

    [[nodiscard]] bool empty() const noexcept { return scripts.empty(); }
};

/// A quest stage's fragment: `function` of `script` runs when the stage is set.
struct QuestFragment {
    std::uint16_t stage{};
    std::int16_t unknown{};
    std::int32_t log_entry{};
    std::uint8_t unknown2{};
    std::string script;
    std::string function;
};

/// The scripts attached to one quest alias.
struct AliasScripts {
    ScriptObject alias; ///< `form` is the owning quest in vanilla.
    std::int16_t version{};
    std::int16_t object_format{};
    std::vector<Script> scripts;
};

/// What follows a QUST's scripts.
struct QuestFragments {
    std::uint8_t unknown{};
    std::string script; ///< The fragment script; empty without fragments.
    std::vector<QuestFragment> fragments;
    std::vector<AliasScripts> aliases;
};

/// Read the script part of a VMAD payload. Stops after the last script;
/// anything after it is left in `body`.
[[nodiscard]] io::ParseResult<ScriptData> read_script_data(io::SpanReader& body);

/// Read QUST fragment data from where `read_script_data` stopped. `scripts`
/// supplies the object format.
[[nodiscard]] io::ParseResult<QuestFragments> read_quest_fragments(io::SpanReader& body,
                                                                   const ScriptData& scripts);

} // namespace bethconv::record
