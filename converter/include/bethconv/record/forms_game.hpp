// SPDX-License-Identifier: GPL-3.0-or-later
//
// Game-data types: GMST and GLOB (settings), ENCH and SPEL (magic), CLAS and
// FACT (actor classification), NPC_ (58 distinct field types in vanilla) and
// QUST.
//
// GMST's DATA layout depends on its editor id's first letter, and EDID is not
// guaranteed to come first, so DATA is captured during the walk and interpreted
// afterwards. No other type needs this.
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

/// GMST: a game setting. 1,659 in SE.
struct GameSetting {
    std::string editor_id;

    /// From the editor id's first letter: `b` bool, `i` int, `f` float, `s`
    /// string, `u` unsigned. Anything else is `unknown` and DATA stays raw.
    enum class Kind : std::uint8_t { unknown, boolean, integer, floating, string, unsigned_int };
    Kind kind{Kind::unknown};

    /// DATA as stored, always kept. 4 bytes on all 1,659 vanilla records, but
    /// only because vanilla is localized; in a non-localized plugin a string
    /// setting holds the text.
    std::vector<std::byte> raw;

    bool boolean_value{};
    std::int32_t integer_value{};
    std::uint32_t unsigned_value{};
    float float_value{};
    LString text;

    static constexpr std::size_t k_scalar_size = 4;

    [[nodiscard]] static Kind kind_of(std::string_view editor_id) noexcept;
};

/// GLOB: a global variable. 1,260 in SE.
struct GlobalVariable {
    std::string editor_id;

    /// FNAM, 1 byte on all 1,260: 's' short, 'l' long, 'f' float. The value is
    /// always stored as a float.
    char kind{};
    float value{}; ///< FLTV, 4 bytes on all 1,260.
};

/// CLAS: an actor class (skill weights for leveling). 159 in SE.
struct ActorClass {
    std::string editor_id;
    LString name;
    LString description;

    /// DATA, 36 bytes on all 159 SE and 138 LE.
    std::uint32_t unknown{};
    std::uint8_t training_skill{};
    std::uint8_t training_level{};
    /// Eighteen skills in game order, one weight byte each.
    static constexpr std::size_t k_skills = 18;
    std::array<std::uint8_t, k_skills> skill_weights{};
    float bleedout_default{};
    std::uint32_t voice_points{};
    std::uint8_t health_weight{};
    std::uint8_t magicka_weight{};
    std::uint8_t stamina_weight{};
    std::uint8_t flags{};
};

/// FACT: a faction (relations, ownership, vendors). 1,424 in SE.
struct Faction {
    std::string editor_id;
    LString name;

    /// XNAM, 12 bytes on all 1,202: other faction, disposition modifier, combat
    /// reaction.
    struct Relation {
        FormId faction;
        std::int32_t modifier{};
        std::uint32_t combat_reaction{};
    };
    std::vector<Relation> relations;
    static constexpr std::size_t k_relation_size = 12;

    std::uint32_t flags{}; ///< DATA, 4 bytes on all 1,424 SE and 1,084 LE.

    FormId prison;        ///< JAIL
    FormId follower_wait; ///< WAIT
    FormId evidence_chest;///< STOL
    FormId belongings;    ///< PLCN
    FormId crime_group;   ///< CRGR
    FormId jail_outfit;   ///< JOUT

    /// CRVA: 20 bytes 1,309 times, 16 bytes 112 times, 12 bytes twice. The
    /// crime-gold block grew over versions; kept raw, the size identifies it.
    std::vector<std::byte> crime_values;

    /// RNAM opens a rank; the following MNAM/FNAM are its titles.
    struct Rank {
        std::uint32_t number{};
        LString male_title;
        LString female_title;
    };
    std::vector<Rank> ranks;

    FormId vendor_list;  ///< VEND
    FormId vendor_chest; ///< VENC
    /// VENV, 12 bytes on all 1,336: trading hours, radius, two flags.
    std::vector<std::byte> vendor_values;
    /// PLVD, 12 bytes on all 299: where to find the vendor chest.
    std::vector<std::byte> vendor_location;

    std::uint32_t declared_condition_count{};         ///< CITC
    std::vector<std::vector<std::byte>> conditions;   ///< CTDA, 32 bytes each.
    std::vector<std::string> condition_strings;       ///< CIS1/CIS2

    /// Source: UESP, FACT record, DATA flags.
    enum class Flag : std::uint32_t {
        hidden_from_pc = 0x0001,
        special_combat = 0x0002,
        track_crime = 0x0040,
        ignore_murder = 0x0080,
        ignore_assault = 0x0100,
        ignore_stealing = 0x0200,
        ignore_trespass = 0x0400,
        do_not_report_crimes_against_members = 0x0800,
        crime_gold_use_defaults = 0x1000,
        ignore_pickpocket = 0x2000,
        vendor = 0x4000,
        can_be_owner = 0x8000,
    };
    [[nodiscard]] bool has(Flag f) const noexcept {
        return (flags & static_cast<std::uint32_t>(f)) != 0;
    }
};

/// ENCH: an object effect (weapon or armor enchantment). 766 in SE.
struct Enchantment {
    std::string editor_id;
    ObjectBounds bounds;
    LString name;

    /// ENIT: 36 bytes 762 times and 32 bytes four times, in LE and SE. The
    /// short form predates 1.70 and lacks the worn restriction, which is
    /// therefore optional.
    std::int32_t cost{};
    std::uint32_t flags{};
    std::uint32_t cast_type{};
    std::int32_t amount{};
    std::uint32_t target_type{};
    std::uint32_t enchantment_type{};
    float charge_time{};
    FormId base_enchantment;
    std::optional<FormId> worn_restrictions;

    static constexpr std::size_t k_enit_short = 32;
    static constexpr std::size_t k_enit_full = 36;

    std::vector<EffectItem> effects; ///< EFID/EFIT/CTDA
};

/// SPEL: spell, shout effect, ability, disease or power. 1,561 in SE.
struct Spell {
    std::string editor_id;
    ObjectBounds bounds;
    LString name;
    LString description;

    FormId menu_display_object; ///< MDOB
    FormId equip_type;          ///< ETYP

    /// SPIT, 36 bytes on all 1,561 SE and 827 LE.
    std::int32_t cost{};
    std::uint32_t flags{};
    std::uint32_t type{};
    float charge_time{};
    std::uint32_t cast_type{};
    std::uint32_t target_type{};
    float cast_duration{};
    float range{};
    FormId half_cost_perk;

    std::vector<EffectItem> effects; ///< EFID/EFIT/CTDA
};

/// NPC_: an actor. 6,626 in SE, all compressed, 58 distinct field types.
struct Npc {
    std::string editor_id;
    ObjectBounds bounds;
    LString name;
    LString short_name; ///< SHRT
    KeywordList keywords;
    Destruction destruction;
    ScriptData scripts;

    /// ACBS, 24 bytes on all 6,626 SE and 5,118 LE.
    std::uint32_t flags{};
    std::uint16_t magicka_offset{};
    std::uint16_t stamina_offset{};
    std::uint16_t level{};
    std::uint16_t calc_min_level{};
    std::uint16_t calc_max_level{};
    std::uint16_t speed_multiplier{};
    std::uint16_t disposition_base{};
    std::uint16_t template_flags{};
    std::uint16_t health_offset{};
    std::uint16_t bleedout_override{};

    /// AIDT, 20 bytes on all 6,626: seven behavior bytes, then three words of
    /// crime response.
    std::uint8_t aggression{};
    std::uint8_t confidence{};
    std::uint8_t energy{};
    std::uint8_t morality{};
    std::uint8_t mood{};
    std::uint8_t assistance{};
    std::uint8_t aggro_radius_behaviour{};
    std::uint8_t aidt_unknown{};
    std::uint32_t warn{};
    std::uint32_t warn_or_attack{};
    std::uint32_t attack{};

    /// DNAM, 52 bytes on all 6,626 SE and 5,118 LE: 18 skill values, 18 skill
    /// offsets, then three derived stats.
    static constexpr std::size_t k_skills = 18;
    std::array<std::uint8_t, k_skills> skill_values{};
    std::array<std::uint8_t, k_skills> skill_offsets{};
    std::uint16_t health{};
    std::uint16_t magicka{};
    std::uint16_t stamina{};
    std::uint16_t dnam_unknown{};
    float far_away_model_distance{};
    std::uint8_t geared_up_weapons{};
    std::vector<std::byte> dnam_tail; ///< Three bytes UESP does not name.

    /// SNAM, 8 bytes each: faction and rank.
    struct FactionRank {
        FormId faction;
        std::int32_t rank{};
    };
    std::vector<FactionRank> factions;
    static constexpr std::size_t k_faction_size = 8;

    /// PRKZ + PRKR, 8 bytes each: perk and rank.
    std::uint32_t declared_perk_count{};
    struct PerkRank {
        FormId perk;
        std::uint32_t rank{};
    };
    std::vector<PerkRank> perks;
    static constexpr std::size_t k_perk_size = 8;

    std::uint32_t declared_spell_count{}; ///< SPCT
    std::vector<FormId> spells;           ///< SPLO
    std::uint32_t declared_item_count{};  ///< COCT
    std::vector<ContainerItem> items;     ///< CNTO (+ COED)
    std::vector<FormId> packages;         ///< PKID
    std::vector<FormId> head_parts;       ///< PNAM

    FormId death_item;      ///< INAM
    FormId voice_type;      ///< VTCK
    FormId npc_template;    ///< TPLT
    FormId race;            ///< RNAM
    FormId actor_class;     ///< CNAM
    FormId worn_armor;      ///< WNAM
    FormId far_away_model;  ///< ANAM
    FormId attack_race;     ///< ATKR
    FormId hair_colour;     ///< HCLF
    FormId combat_style;    ///< ZNAM
    FormId gift_filter;     ///< GNAM
    FormId audio_template;  ///< CSCR
    FormId default_outfit;  ///< DOFT
    FormId sleeping_outfit; ///< SOFT
    FormId default_package_list; ///< DPLT
    FormId crime_faction;   ///< CRIF
    FormId head_texture;    ///< FTST
    FormId sleeping_override;  ///< SPOR
    FormId combat_override;    ///< ECOR

    float height{};                 ///< NAM6
    float weight{};                 ///< NAM7
    std::uint32_t sound_level{};    ///< NAM8
    std::uint16_t nam5{};           ///< NAM5, 0xFFFF on every vanilla record.
    /// QNAM, 12 bytes on all 6,626: RGB text color.
    float text_red{};
    float text_green{};
    float text_blue{};

    /// NAM9 (76 bytes on 3,493) and NAMA (16 bytes on 3,433): FaceGen morph
    /// values and face part indices. Kept raw.
    std::vector<std::byte> face_morph;
    std::vector<std::byte> face_parts;

    /// ATKD + ATKE: 44-byte attack data and the animation event it fires.
    struct Attack {
        std::vector<std::byte> data;
        std::string event;
    };
    std::vector<Attack> attacks;

    /// TINI/TINC/TINV/TIAS: one tint layer (39,708 of each in SE).
    struct TintLayer {
        std::uint16_t index{};   ///< TINI
        std::uint32_t colour{};  ///< TINC, RGBA.
        std::uint32_t value{};   ///< TINV
        std::int16_t preset{};   ///< TIAS
    };
    std::vector<TintLayer> tint_layers;

    /// CSDT/CSDI/CSDC: sound type, its sounds, their chances.
    struct SoundSet {
        std::uint32_t type{};        ///< CSDT
        std::vector<FormId> sounds;  ///< CSDI
        std::vector<std::uint8_t> chances; ///< CSDC
    };
    std::vector<SoundSet> sounds;

    /// DATA is 0 bytes on all 6,626: a marker, stored as a boolean so the
    /// census does not count it as unhandled.
    bool has_data_marker{};
};

/// QUST: a quest, with its stages, objectives and aliases. 1,811 in Skyrim.esm.
///
/// Fields are positional: INDX opens a stage and QSDT a log entry in it, QOBJ
/// an objective and QSTA a target in it, ALST (reference) or ALLS (location)
/// an alias that ALED closes. CTDA/CIS1/CIS2 belong to the innermost open
/// one; before any of them, to the dialogue conditions, and after NEXT to the
/// event conditions. FNAM is an objective's or an alias's flags.
struct Quest {
    std::string editor_id;
    LString name;
    ScriptData scripts;
    QuestFragments fragments; ///< The rest of VMAD.

    /// DNAM, 12 bytes on all 1,811.
    std::uint16_t flags{};
    std::uint8_t priority{};
    std::uint8_t form_version{};
    std::uint32_t dnam_unknown{};
    std::uint32_t type{};

    FourCC event;                    ///< ENAM: the story manager event.
    std::vector<FormId> text_globals;///< QTGL
    std::string filter;              ///< FLTR: the editor's folder.
    std::uint32_t next_alias_id{};   ///< ANAM
    bool has_next{};                 ///< NEXT, a marker.

    struct Conditions {
        std::vector<std::vector<std::byte>> raw; ///< CTDA, 32 bytes each.
        std::vector<std::string> strings;        ///< CIS1/CIS2
    };
    Conditions dialogue_conditions;
    Conditions event_conditions;

    struct LogEntry {
        std::uint8_t flags{}; ///< QSDT
        Conditions conditions;
        LString text;         ///< CNAM, a journal entry.
        FormId next_quest;    ///< NAM0
        /// SCHR/SCTX/QNAM: an unused result-script block (25 in Skyrim.esm).
        std::vector<std::byte> script_header;
        std::string script_text;
        std::vector<FormId> script_refs;

        enum class Flag : std::uint8_t { complete_quest = 0x01, fail_quest = 0x02 };
        [[nodiscard]] bool has(Flag f) const noexcept {
            return (flags & static_cast<std::uint8_t>(f)) != 0;
        }
    };
    struct Stage {
        std::uint16_t index{}; ///< INDX[0..1]
        std::uint8_t flags{};  ///< INDX[2]
        std::uint8_t unknown{};
        std::vector<LogEntry> log;

        enum class Flag : std::uint8_t { start_up = 0x02, shut_down = 0x04, keep_instance = 0x08 };
        [[nodiscard]] bool has(Flag f) const noexcept {
            return (flags & static_cast<std::uint8_t>(f)) != 0;
        }
    };
    std::vector<Stage> stages;

    struct Target {
        std::int32_t alias{}; ///< QSTA[0..3]
        std::uint32_t flags{};
        Conditions conditions;
    };
    struct Objective {
        std::uint16_t index{}; ///< QOBJ
        std::uint32_t flags{}; ///< FNAM
        LString text;          ///< NNAM
        std::vector<Target> targets;
    };
    std::vector<Objective> objectives;

    struct Alias {
        bool location{};       ///< ALLS rather than ALST.
        std::uint32_t id{};
        std::string name;      ///< ALID
        std::uint32_t flags{}; ///< FNAM
        std::int32_t force_into{-1};    ///< ALFI
        FormId specific_location;       ///< ALFL
        FormId forced_ref;              ///< ALFR
        FormId unique_actor;            ///< ALUA
        std::int32_t near_alias{-1};    ///< ALNA
        std::uint32_t near_type{};      ///< ALNT
        FormId created_object;          ///< ALCO
        std::uint32_t create_at{};      ///< ALCA: alias (low 16 bits) and how.
        std::uint32_t create_level{};   ///< ALCL
        FormId external_quest;          ///< ALEQ
        std::int32_t external_alias{-1};///< ALEA
        std::int32_t location_alias{-1};///< ALFA: from a location alias.
        FormId location_ref_type;       ///< ALRT
        FourCC from_event;              ///< ALFE
        std::uint32_t event_data{};     ///< ALFD
        FormId keyword;                 ///< KNAM
        FormId display_name;            ///< ALDN
        FormId voice_types;             ///< VTCK
        FormId sleep_override;          ///< SPOR
        FormId observe_corpse_override; ///< OCOR
        FormId guard_warn_override;     ///< GWOR
        FormId combat_override;         ///< ECOR
        std::vector<FormId> spells;     ///< ALSP
        std::vector<FormId> factions;   ///< ALFC
        std::vector<FormId> packages;   ///< ALPC
        KeywordList keywords;
        std::uint32_t declared_item_count{};
        std::vector<ContainerItem> items;
        Conditions conditions;

        /// Source: UESP, QUST record, alias FNAM.
        enum class Flag : std::uint32_t {
            reserves = 0x0001,
            optional = 0x0002,
            quest_object = 0x0004,
            allow_reuse = 0x0008,
            allow_dead = 0x0010,
            in_loaded_area = 0x0020,
            essential = 0x0040,
            allow_disabled = 0x0080,
            stores_text = 0x0100,
            allow_reserved = 0x0200,
            protected_ = 0x0400,
            allow_destroyed = 0x1000,
            closest = 0x2000,
            uses_stored_text = 0x4000,
            initially_disabled = 0x8000,
            allow_cleared = 0x10000,
            clear_names = 0x20000,
        };
        [[nodiscard]] bool has(Flag f) const noexcept {
            return (flags & static_cast<std::uint32_t>(f)) != 0;
        }
    };
    std::vector<Alias> aliases;

    /// Source: UESP, QUST record, DNAM flags.
    enum class Flag : std::uint16_t {
        start_game_enabled = 0x0001,
        wilderness_encounter = 0x0004,
        allow_repeated_stages = 0x0008,
        run_once = 0x0100,
        exclude_from_export = 0x0200,
        warn_on_alias_fill_failure = 0x0400,
    };
    [[nodiscard]] bool has(Flag f) const noexcept {
        return (flags & static_cast<std::uint16_t>(f)) != 0;
    }
};

// ---- parsing --------------------------------------------------------------

[[nodiscard]] io::ParseResult<GameSetting> parse_game_setting(io::SpanReader& data,
                                                              const FormContext& ctx);
[[nodiscard]] io::ParseResult<GlobalVariable> parse_global(io::SpanReader& data,
                                                           const FormContext& ctx);
[[nodiscard]] io::ParseResult<ActorClass> parse_actor_class(io::SpanReader& data,
                                                            const FormContext& ctx);
[[nodiscard]] io::ParseResult<Faction> parse_faction(io::SpanReader& data,
                                                     const FormContext& ctx);
[[nodiscard]] io::ParseResult<Enchantment> parse_enchantment(io::SpanReader& data,
                                                             const FormContext& ctx);
[[nodiscard]] io::ParseResult<Spell> parse_spell(io::SpanReader& data, const FormContext& ctx);
[[nodiscard]] io::ParseResult<Npc> parse_npc(io::SpanReader& data, const FormContext& ctx);
[[nodiscard]] io::ParseResult<Quest> parse_quest(io::SpanReader& data, const FormContext& ctx);

} // namespace bethconv::record
