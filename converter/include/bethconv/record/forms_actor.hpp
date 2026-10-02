// SPDX-License-Identifier: GPL-3.0-or-later
//
// Field definitions for what an actor looks like: RACE (skeleton, skin,
// behaviour and body parts per sex), ARMO (what is worn) and ARMA (the model
// per race and sex), OTFT (outfits) and LVLI (leveled items, which outfits
// list). Counts are vanilla SE (Skyrim.esm, Update and the three DLC).
#pragma once

#include "bethconv/io/parse_error.hpp"
#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/plugin.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bethconv::record {

/// BOD2 (8 bytes) or the older BODT (12 bytes, or 8 on 9 ARMO): the body
/// slots an item covers (bit n is slot 30 + n) and its armor type.
struct BodyTemplate {
    std::uint32_t slots{};
    std::uint32_t flags{};      ///< BODT only.
    std::uint32_t armor_type{}; ///< 0 light, 1 heavy, 2 clothing.
    bool present{};
};

/// A field kept as it was: its type and bytes.
struct RawField {
    FourCC type;
    std::vector<std::byte> bytes;
};

/// ARMO: something worn. 3,835 in SE.
struct Armor {
    std::string editor_id;
    ObjectBounds bounds;
    LString name;
    KeywordList keywords;
    ScriptData scripts;
    BodyTemplate body;
    ModelData male_model;   ///< MOD2: the ground model, male.
    ModelData female_model; ///< MOD4
    LString description;    ///< DESC
    FormId enchantment;     ///< EITM
    FormId equip_slot;      ///< ETYP
    FormId bash_impact;     ///< BIDS
    FormId bash_material;   ///< BAMT
    FormId pickup_sound;    ///< YNAM
    FormId drop_sound;      ///< ZNAM
    FormId race;            ///< RNAM
    FormId template_armor;  ///< TNAM
    std::uint32_t value{};  ///< DATA
    float weight{};         ///< DATA
    std::uint32_t armor_rating{}; ///< DNAM, x100.
    /// MODL, one per addon (5,786 in SE): the ARMA that render this item.
    std::vector<FormId> addons;
};

/// ARMA: an armor addon, the model one race wears. 1,113 in SE.
struct ArmorAddon {
    std::string editor_id;
    BodyTemplate body;
    FormId race;                   ///< RNAM
    /// DNAM, 12 bytes on all 1,113.
    std::uint8_t male_priority{};
    std::uint8_t female_priority{};
    std::uint8_t male_weight_slider{};  ///< Bit 1: _0/_1 weight variants exist.
    std::uint8_t female_weight_slider{};
    std::uint16_t dnam_unknown{};
    std::uint8_t detection_sound{};
    std::uint8_t dnam_unknown2{};
    float weapon_adjust{};
    ModelData male_model;          ///< MOD2: third person, male.
    ModelData female_model;        ///< MOD3
    ModelData male_first_person;   ///< MOD4
    ModelData female_first_person; ///< MOD5
    FormId male_skin_texture;      ///< NAM0
    FormId female_skin_texture;    ///< NAM1
    FormId male_skin_swap;         ///< NAM2
    FormId female_skin_swap;       ///< NAM3
    FormId footstep_sound;         ///< SNDD
    FormId art_object;             ///< ONAM
    /// MODL, 4 bytes each (13,961 in SE): further races that may wear it.
    std::vector<FormId> additional_races;
};

/// OTFT: an outfit, a list of ARMO and LVLI. 610 in SE.
struct Outfit {
    std::string editor_id;
    std::vector<FormId> items; ///< INAM
};

/// LVLI: a leveled item list. 3,846 in SE, 25,532 entries.
struct LeveledItem {
    std::string editor_id;
    ObjectBounds bounds;
    std::uint8_t chance_none{};    ///< LVLD
    std::uint8_t flags{};          ///< LVLF: 1 all levels, 2 each, 4 use all.
    FormId chance_global;          ///< LVLG
    std::uint8_t declared_count{}; ///< LLCT
    struct Entry {
        std::uint16_t level{};
        std::uint16_t unknown{};
        FormId reference;
        std::uint16_t count{};
        std::uint16_t unknown2{};
    };
    std::vector<Entry> entries; ///< LVLO, 12 bytes on all 25,532.
};

/// RACE: 204 in SE. Its fields come in sections: NAM0 opens head data, NAM1
/// body data, NAM3 behaviour; within one, MNAM and FNAM switch between male
/// and female. Only what building an actor needs is read; every other field
/// is kept in `other`, so nothing is dropped.
struct Race {
    std::string editor_id;
    LString name;
    LString description;
    KeywordList keywords;
    std::uint32_t declared_spell_count{}; ///< SPCT
    std::vector<FormId> spells;           ///< SPLO
    FormId skin;                          ///< WNAM: the ARMO worn when naked.
    BodyTemplate body;
    /// DATA, 164 bytes on all 204: kept whole; the heights and flags read.
    std::vector<std::byte> data;
    std::array<float, 2> height{1, 1}; ///< male, female
    std::array<float, 2> weight{1, 1};
    std::uint32_t flags{};             ///< DATA flags: 1 playable, …

    /// Per sex (0 male, 1 female).
    struct Sex {
        std::string skeleton;   ///< ANAM: e.g. actors\character\character assets\skeleton.nif
        std::string behaviour;  ///< MODL in the NAM3 section: the behaviour graph (.hkx)
        /// NAM1 section: INDX (0 upper body, 1 lower, 2 hand, 3 foot, 4 tail)
        /// and its MODL.
        struct BodyPart {
            std::uint32_t index{};
            std::string model;
        };
        std::vector<BodyPart> body_parts;
        std::vector<FormId> head_parts; ///< HEAD in the NAM0 section
    };
    std::array<Sex, 2> sexes;

    FormId voice_male;      ///< VTCK, first of two
    FormId voice_female;
    FormId morph_race;      ///< NAM8
    FormId armor_race;      ///< RNAM

    std::vector<RawField> other;
};

[[nodiscard]] io::ParseResult<Armor> parse_armor(io::SpanReader& data, const FormContext& ctx);
[[nodiscard]] io::ParseResult<ArmorAddon> parse_armor_addon(io::SpanReader& data,
                                                            const FormContext& ctx);
[[nodiscard]] io::ParseResult<Outfit> parse_outfit(io::SpanReader& data, const FormContext& ctx);
[[nodiscard]] io::ParseResult<LeveledItem> parse_leveled_item(io::SpanReader& data,
                                                              const FormContext& ctx);
[[nodiscard]] io::ParseResult<Race> parse_race(io::SpanReader& data, const FormContext& ctx);

} // namespace bethconv::record
