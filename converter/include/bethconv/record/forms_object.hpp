// SPDX-License-Identifier: GPL-3.0-or-later
//
// Placeable base objects a REFR can point at: what furnishes a cell. Mostly the
// same shape: model, bounds, name, keywords, plus a few type-specific values.
//
// Same rules as forms.hpp (measured sizes, unknown fields tallied, plugin-local
// FormIDs), plus one more: blocks nothing reads yet are kept raw rather than
// partially named. WEAP DNAM is 100 bytes on all 3,359 vanilla weapons and
// UESP names about half; a half-named struct hides which half is wrong.
//
// Source: <https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format>, checked
// against xEdit.
#pragma once

#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/plugin.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bethconv::record {

/// TXST: a texture set, up to eight maps. 790 in SE.
struct TextureSet {
    std::string editor_id;
    ObjectBounds bounds;

    /// TX00..TX07, zstring paths under textures/. The slot defines the use: 0
    /// diffuse, 1 normal, 2 environment mask / subsurface tint, 3 glow / detail,
    /// 4 height, 5 environment, 6 multilayer, 7 backlight mask or specular.
    /// TX06 never occurs in vanilla.
    static constexpr std::size_t k_slots = 8;
    std::array<std::string, k_slots> textures;

    std::uint16_t flags{}; ///< DNAM, 2 bytes on 785 of 790.

    /// DODT, 36 bytes on all 59 that have it: decal placement (nine words).
    /// Kept raw.
    std::vector<std::byte> decal;

    /// Source: UESP, TXST record, DNAM.
    enum class Flag : std::uint16_t {
        no_specular_map = 0x0001,
        facegen_textures = 0x0002,
        has_model_space_normal_map = 0x0004,
    };
    [[nodiscard]] bool has(Flag f) const noexcept {
        return (flags & static_cast<std::uint16_t>(f)) != 0;
    }
};

/// ACTI: an activator (lever, chest lid, wall of water). 2,713 in SE, 793
/// destructible.
struct Activator {
    std::string editor_id;
    ObjectBounds bounds;
    ModelData model;
    LString name;
    KeywordList keywords;
    Destruction destruction;
    ScriptData scripts;

    std::uint32_t marker_colour{}; ///< PNAM, RGBA, one byte each.
    FormId looping_sound;          ///< SNAM
    FormId activation_sound;       ///< VNAM
    FormId water_type;             ///< WNAM
    LString activate_text;         ///< RNAM, overrides "Open"/"Search".
    std::uint16_t flags{};         ///< FNAM, 2 bytes on 2,596 of 2,713.
    FormId interaction_keyword;    ///< KNAM

    /// Source: UESP, ACTI record, FNAM.
    enum class Flag : std::uint16_t {
        no_displacement = 0x0001,
        ignored_by_sandbox = 0x0002,
    };
    [[nodiscard]] bool has(Flag f) const noexcept {
        return (flags & static_cast<std::uint16_t>(f)) != 0;
    }
};

/// CONT: a container. 629 in SE, 13,038 items in total.
struct Container {
    std::string editor_id;
    ObjectBounds bounds;
    ModelData model;
    LString name;
    Destruction destruction;
    ScriptData scripts;

    std::uint32_t declared_item_count{}; ///< COCT
    std::vector<ContainerItem> items;    ///< CNTO (+ COED)

    /// DATA, 5 bytes on all 629: flags byte, then float weight. Read field by
    /// field since 5 is not a multiple of 4.
    std::uint8_t flags{};
    float weight{};

    FormId open_sound;  ///< SNAM
    FormId close_sound; ///< QNAM

    /// Source: UESP, CONT record, DATA flags.
    enum class Flag : std::uint8_t {
        allow_sounds_when_animation = 0x01,
        respawns = 0x02,
        show_owner = 0x04,
    };
    [[nodiscard]] bool has(Flag f) const noexcept {
        return (flags & static_cast<std::uint8_t>(f)) != 0;
    }
};

/// MISC: miscellaneous items (gold, gems, tools, clutter). 1,063 in SE.
struct MiscItem {
    std::string editor_id;
    ObjectBounds bounds;
    ModelData model;
    LString name;
    KeywordList keywords;
    ScriptData scripts;

    std::string icon;      ///< ICON, on one vanilla record.
    std::int32_t value{};  ///< DATA[0..3], 8 bytes on all 1,063.
    float weight{};        ///< DATA[4..7]
    FormId pickup_sound;   ///< YNAM
    FormId drop_sound;     ///< ZNAM
};

/// MSTT: a movable static. 838 in SE.
struct MovableStatic {
    std::string editor_id;
    ObjectBounds bounds;
    ModelData model;
    Destruction destruction;

    std::uint8_t flags{};  ///< DATA, 1 byte on all 838.
    FormId ambient_sound;  ///< SNAM
};

/// FURN: furniture (bed, chair, workbench) with the markers actors use. 517 in
/// SE.
struct Furniture {
    std::string editor_id;
    ObjectBounds bounds;
    ModelData model;
    LString name;
    KeywordList keywords;
    Destruction destruction;
    ScriptData scripts;

    std::uint16_t flags{};        ///< FNAM, 2 bytes on all 517.
    FormId interaction_keyword;   ///< KNAM
    std::uint32_t marker_flags{}; ///< MNAM, 4 bytes on all 517.
    std::uint8_t bench_type{};    ///< WBDT[0], 2 bytes on all 517.
    std::uint8_t bench_skill{};   ///< WBDT[1]

    /// Marker table: spots an actor can occupy and what it can do there. Four
    /// parallel fields, one entry per marker, plus optional model overrides.
    std::vector<std::int32_t> marker_indices;   ///< ENAM
    std::vector<std::uint32_t> marker_flags2;   ///< NAM0
    std::vector<std::uint32_t> entry_points;    ///< FNPR, 4 bytes, 619 times.
    std::vector<FormId> marker_keywords;        ///< FNMK
    std::vector<std::string> marker_models;     ///< XMRK

    /// PNAM, 4 bytes on all 517; not named by UESP or xEdit, kept raw.
    std::uint32_t pnam{};
};

/// FLOR: a harvestable (plant, mushroom, ore vein). 122 in SE.
struct Flora {
    std::string editor_id;
    ObjectBounds bounds;
    ModelData model;
    LString name;
    KeywordList keywords;
    ScriptData scripts;

    std::uint32_t pnam{};    ///< PNAM, 4 bytes on all 122; undocumented.
    LString activate_text;   ///< RNAM
    std::uint16_t flags{};   ///< FNAM, 2 bytes on all 122.
    FormId harvest_sound;    ///< SNAM
    FormId ingredient;       ///< PFIG, the harvested item.

    /// PFPC, 4 bytes on all 122: yield in spring, summer, autumn, winter.
    std::array<std::uint8_t, 4> seasonal_yield{};
};

/// TREE: a harvestable with wind animation data. 288 in SE.
struct Tree {
    std::string editor_id;
    ObjectBounds bounds;
    ModelData model;
    LString name;
    ScriptData scripts; ///< VMAD; none in vanilla, 332 in the FUS list.

    FormId ingredient;    ///< PFIG
    FormId harvest_sound; ///< SNAM
    std::array<std::uint8_t, 4> seasonal_yield{}; ///< PFPC

    /// CNAM, 48 bytes on all 288: twelve floats of wind response. Only trunk
    /// and branch flexibility are documented; the rest is kept raw.
    float trunk_flexibility{};
    float branch_flexibility{};
    std::vector<std::byte> cnam_rest;
};

/// KEYM: a key. 381 in SE.
struct Key {
    std::string editor_id;
    ObjectBounds bounds;
    ModelData model;
    LString name;
    KeywordList keywords;
    ScriptData scripts;

    std::int32_t value{}; ///< DATA[0..3], 8 bytes on all 381.
    float weight{};       ///< DATA[4..7]
    FormId pickup_sound;  ///< YNAM
    FormId drop_sound;    ///< ZNAM
};

/// ALCH: potion, poison or food. 623 in SE.
struct Ingestible {
    std::string editor_id;
    ObjectBounds bounds;
    ModelData model;
    LString name;
    KeywordList keywords;

    float weight{};      ///< DATA, 4 bytes on all 623 (weight only).
    FormId pickup_sound; ///< YNAM
    FormId drop_sound;   ///< ZNAM

    /// ENIT, 20 bytes on all 623 in SE and all 363 in LE.
    std::int32_t value{};
    std::uint32_t flags{};
    FormId addiction;
    std::uint32_t addiction_chance{};
    FormId use_sound;

    std::vector<EffectItem> effects; ///< EFID/EFIT/CTDA

    /// Source: UESP, ALCH record, ENIT flags.
    enum class Flag : std::uint32_t {
        no_auto_calc = 0x00000001,
        food_item = 0x00000002,
        medicine = 0x00010000,
        poison = 0x00020000,
    };
    [[nodiscard]] bool has(Flag f) const noexcept {
        return (flags & static_cast<std::uint32_t>(f)) != 0;
    }
};

/// AMMO: arrows and bolts. 78 in SE, 35 in LE.
struct Ammo {
    std::string editor_id;
    ObjectBounds bounds;
    ModelData model;
    LString name;
    LString description;
    KeywordList keywords;

    FormId pickup_sound; ///< YNAM
    FormId drop_sound;   ///< ZNAM

    /// DATA is 16 bytes in LE (all 35) and 20 in SE (all 78): projectile,
    /// flags, damage, value, plus a float weight in SE. `weight` is nullopt for
    /// LE rather than 0, since the file does not say.
    FormId projectile;
    std::uint32_t flags{};
    float damage{};
    std::int32_t value{};
    std::optional<float> weight;

    static constexpr std::size_t k_data_le = 16;
    static constexpr std::size_t k_data_se = 20;

    /// Source: UESP, AMMO record, DATA flags.
    enum class Flag : std::uint32_t {
        ignores_normal_weapon_resistance = 0x01,
        non_playable = 0x02,
        non_bolt = 0x04,
    };
    [[nodiscard]] bool has(Flag f) const noexcept {
        return (flags & static_cast<std::uint32_t>(f)) != 0;
    }
};

/// WEAP: a weapon. 3,359 in SE.
struct Weapon {
    std::string editor_id;
    ObjectBounds bounds;
    ModelData model;
    LString name;
    LString description;
    KeywordList keywords;
    ScriptData scripts;

    /// DATA, 10 bytes on all 3,359 SE and 2,484 LE weapons: value, weight,
    /// damage.
    std::int32_t value{};
    float weight{};
    std::int16_t damage{};

    /// DNAM is 100 bytes on every vanilla weapon; CRDT is 16 bytes in LE and 24
    /// in SE. Both kept raw: UESP documents only about half of DNAM.
    std::vector<std::byte> weapon_data;   ///< DNAM
    std::vector<std::byte> critical_data; ///< CRDT

    static constexpr std::size_t k_crdt_le = 16;
    static constexpr std::size_t k_crdt_se = 24;

    std::uint32_t detection_sound_level{}; ///< VNAM
    FormId template_weapon;                ///< CNAM
    FormId enchantment;                    ///< EITM
    std::uint16_t enchantment_amount{};    ///< EAMT
    FormId equip_type;                     ///< ETYP
    FormId block_bash_impact;              ///< BIDS
    FormId block_material;                 ///< BAMT
    FormId impact_data_set;                ///< INAM
    FormId first_person_model;             ///< WNAM
    FormId attack_sound;                   ///< SNAM
    FormId attack_sound_2d;                ///< XNAM
    FormId attack_loop_sound;              ///< NAM7
    FormId attack_fail_sound;              ///< TNAM
    FormId idle_sound;                     ///< UNAM
    FormId equip_sound;                    ///< NAM9
    FormId unequip_sound;                  ///< NAM8
    std::string attach_node;               ///< NNAM, on 11 vanilla records.
};

/// PROJ: a projectile in flight (arrow, spell, dart). 248 in SE.
struct Projectile {
    std::string editor_id;
    ObjectBounds bounds;
    ModelData model;
    LString name;
    Destruction destruction;

    /// DATA: 92 bytes 242 times, 84 four times, 88 twice (same widths in LE).
    /// Twenty-three words, about two thirds documented. Kept raw; the width
    /// identifies the variant.
    std::vector<std::byte> data;

    std::string muzzle_flash_model;            ///< NAM1
    std::vector<std::byte> muzzle_flash_hashes;///< NAM2, a MODT-shaped block.
    std::uint32_t sound_level{};               ///< VNAM
};

/// IDLM: an idle marker where an actor plays an animation. 94 in SE.
struct IdleMarker {
    std::string editor_id;
    ObjectBounds bounds;

    std::uint8_t flags{};        ///< IDLF, 1 byte on 90 of 94.
    std::uint8_t declared_count{}; ///< IDLC, the file's count of IDLA entries.
    float timer{};               ///< IDLT
    std::vector<FormId> idles;   ///< IDLA, IDLE FormIDs.
};

/// LVLN: a leveled NPC list. 691 in SE, 4,074 entries.
struct LeveledNpc {
    std::string editor_id;
    ObjectBounds bounds;
    ModelData model;

    std::uint8_t chance_none{};    ///< LVLD, 1 byte on all 691.
    std::uint8_t flags{};          ///< LVLF, 1 byte on all 691.
    std::uint8_t declared_count{}; ///< LLCT

    /// LVLO, 12 bytes on all 4,074 entries in SE and 3,141 in LE.
    struct Entry {
        std::uint16_t level{};
        std::uint16_t unknown{};
        FormId reference;
        std::uint16_t count{};
        std::uint16_t unknown2{};
    };
    std::vector<Entry> entries;

    /// COED, on one vanilla entry: ownership for the preceding entry.
    std::optional<ContainerItem::Extra> extra;

    /// Source: UESP, leveled list LVLF flags.
    enum class Flag : std::uint8_t {
        calculate_from_all_levels_below = 0x01,
        calculate_for_each_item = 0x02,
        use_all = 0x04,
    };
    [[nodiscard]] bool has(Flag f) const noexcept {
        return (flags & static_cast<std::uint8_t>(f)) != 0;
    }
};

// ---- parsing --------------------------------------------------------------

[[nodiscard]] io::ParseResult<TextureSet> parse_texture_set(io::SpanReader& data,
                                                            const FormContext& ctx);
[[nodiscard]] io::ParseResult<Activator> parse_activator(io::SpanReader& data,
                                                         const FormContext& ctx);
[[nodiscard]] io::ParseResult<Container> parse_container(io::SpanReader& data,
                                                         const FormContext& ctx);
[[nodiscard]] io::ParseResult<MiscItem> parse_misc_item(io::SpanReader& data,
                                                        const FormContext& ctx);
[[nodiscard]] io::ParseResult<MovableStatic> parse_movable_static(io::SpanReader& data,
                                                                  const FormContext& ctx);
[[nodiscard]] io::ParseResult<Furniture> parse_furniture(io::SpanReader& data,
                                                         const FormContext& ctx);
[[nodiscard]] io::ParseResult<Flora> parse_flora(io::SpanReader& data, const FormContext& ctx);
[[nodiscard]] io::ParseResult<Tree> parse_tree(io::SpanReader& data, const FormContext& ctx);
[[nodiscard]] io::ParseResult<Key> parse_key(io::SpanReader& data, const FormContext& ctx);
[[nodiscard]] io::ParseResult<Ingestible> parse_ingestible(io::SpanReader& data,
                                                           const FormContext& ctx);
[[nodiscard]] io::ParseResult<Ammo> parse_ammo(io::SpanReader& data, const FormContext& ctx);
[[nodiscard]] io::ParseResult<Weapon> parse_weapon(io::SpanReader& data, const FormContext& ctx);
[[nodiscard]] io::ParseResult<Projectile> parse_projectile(io::SpanReader& data,
                                                           const FormContext& ctx);
[[nodiscard]] io::ParseResult<IdleMarker> parse_idle_marker(io::SpanReader& data,
                                                            const FormContext& ctx);
[[nodiscard]] io::ParseResult<LeveledNpc> parse_leveled_npc(io::SpanReader& data,
                                                            const FormContext& ctx);

} // namespace bethconv::record
