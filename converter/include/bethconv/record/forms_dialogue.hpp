// SPDX-License-Identifier: GPL-3.0-or-later
//
// Dialogue and what its conditions read: DLBR (branches), DIAL (topics), INFO
// (the lines), VTYP (voice types), RELA (relationships) and SCEN (scenes).
//
// DIAL's children, the INFOs, are not part of its payload: they lie in the
// group that follows it (group type 7, label = the DIAL), which the merge
// reports as `MergedRecord::parent`. SSE INFOs have no PNAM (the previous
// INFO); the group order is the order the Creation Kit shows.
//
// Source: UESP (Skyrim_Mod:Mod_File_Format/DIAL, /INFO, /DLBR) and xEdit's
// wbDefinitionsTES5.pas, checked against Skyrim.esm.
#pragma once

#include "bethconv/record/conditions.hpp"
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/plugin.hpp"
#include "bethconv/record/vmad.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bethconv::record {

/// DLBR: a dialogue branch, the unit the quest editor groups topics in. 3,061
/// in Skyrim.esm.
struct DialogueBranch {
    std::string editor_id;
    FormId quest;            ///< QNAM
    FormId start_topic;      ///< SNAM
    std::uint32_t flags{};   ///< DNAM: 1 top-level, 2 blocking, 4 exclusive.
    std::uint32_t category{};///< TNAM: 0 player, 1 command.
};

/// DIAL: a topic. 15,037 in Skyrim.esm.
struct DialogueTopic {
    std::string editor_id;
    LString text;            ///< FULL: the player's line.
    FormId quest;            ///< QNAM
    FormId branch;           ///< BNAM
    float priority{};        ///< PNAM
    /// DATA, 4 bytes on all 15,037: flags (1 do all before repeating),
    /// category, subtype.
    std::uint8_t flags{};
    std::uint8_t category{};
    std::uint16_t subtype{};
    FourCC subtype_code;     ///< SNAM: GREE, HELO, IDLE, ...
    std::uint32_t info_count{}; ///< TIFC
};

/// One spoken response of an INFO: TRDT, then NAM1 (text), NAM2 (notes) and
/// NAM3 (unused).
struct DialogueResponse {
    std::uint32_t emotion{};       ///< TRDT[0]
    std::uint32_t emotion_value{}; ///< TRDT[4]
    std::uint8_t number{};         ///< TRDT[12]
    FormId sound;                  ///< TRDT[16], a SNDR.
    std::uint8_t flags{};          ///< TRDT[20]: 1 use emotion animation.
    LString text;                  ///< NAM1, from the ILSTRINGS table.
    std::string notes;             ///< NAM2

    static constexpr std::size_t k_size = 24;
};

/// INFO: one line of a topic. 31,465 in Skyrim.esm.
struct DialogueInfo {
    std::string editor_id;
    std::uint32_t flags{};         ///< ENAM, low 16 bits.
    std::uint16_t reset_hours{};   ///< ENAM, high 16 bits.
    std::uint8_t favor_level{};    ///< CNAM
    LString prompt;                ///< RNAM
    FormId speaker;                ///< ANAM
    FormId shared_info;            ///< DNAM
    std::vector<FormId> links;     ///< TCLT, in order.
    FormId walk_away_topic;        ///< TWAT
    FormId audio_output;           ///< ONAM
    std::vector<Condition> conditions;
    std::uint32_t declared_condition_count{};
    std::vector<DialogueResponse> responses;
    ScriptData scripts;
    InfoFragments fragments;
};

/// VTYP: a voice type. 143 in Skyrim.esm.
struct VoiceType {
    std::string editor_id;
    std::uint8_t flags{}; ///< DNAM: 1 allow default dialogue, 2 female.
};

/// RELA: a relationship between two NPCs. 642 in Skyrim.esm.
struct Relationship {
    std::string editor_id;
    FormId parent;                 ///< DATA[0]
    FormId child;                  ///< DATA[4]
    /// DATA[8]: 0 lover, 1 ally, 2 confidant, 3 friend, 4 acquaintance, 5
    /// rival, 6 foe, 7 enemy, 8 archnemesis (xEdit).
    std::uint16_t rank{};
    FormId association;            ///< DATA[12], an ASTP.
    static constexpr std::size_t k_size = 16;
};

/// One phase of a scene: HNAM, NAM0, the start conditions, NEXT, the
/// completion conditions, NEXT, the old-style result script (skipped), HNAM.
struct ScenePhase {
    std::string name; ///< NAM0
    std::vector<Condition> start_conditions;
    std::vector<Condition> completion_conditions;
};

/// An actor of a scene: ALID, LNAM, DNAM.
struct SceneActor {
    std::int32_t alias{-1};
    std::uint32_t flags{};           ///< LNAM
    std::uint32_t behaviour_flags{}; ///< DNAM
};

/// One action: ANAM (type) ... ANAM (end). 0 dialogue, 1 package, 2 timer. No
/// action has conditions in Skyrim.esm or Update.esm.
struct SceneAction {
    std::uint16_t type{};
    std::string name;              ///< NAM0
    std::int32_t alias{-1};        ///< ALID
    std::uint32_t index{};         ///< INAM
    std::uint32_t flags{};         ///< FNAM
    std::uint32_t start_phase{};   ///< SNAM
    std::uint32_t end_phase{};     ///< ENAM
    FormId topic;                  ///< DATA (dialogue)
    std::vector<FormId> packages;  ///< PNAM (package)
    float timer_seconds{};         ///< the second SNAM (timer)
    std::int32_t head_track_alias{-1}; ///< HTID
    float delay_max{};             ///< DMAX
    float delay_min{};             ///< DMIN
    std::uint32_t emotion{};       ///< DEMO
    std::uint32_t emotion_value{}; ///< DEVA
};

/// SCEN: a scene. 1,706 in Skyrim.esm.
struct Scene {
    std::string editor_id;
    /// FNAM: 0x1 begin on quest start, 0x2 stop on quest end, 0x4 show all
    /// text, 0x8 repeat while true, 0x10 interruptible.
    std::uint32_t flags{};
    FormId quest; ///< The PNAM after the last action.
    std::vector<Condition> conditions;
    std::uint32_t declared_condition_count{};
    std::vector<ScenePhase> phases;
    std::vector<SceneActor> actors;
    std::vector<SceneAction> actions;
    ScriptData scripts;
    SceneFragments fragments;
};

[[nodiscard]] io::ParseResult<Scene> parse_scene(io::SpanReader& data, const FormContext& ctx);

[[nodiscard]] io::ParseResult<DialogueBranch> parse_dialogue_branch(io::SpanReader& data,
                                                                    const FormContext& ctx);
[[nodiscard]] io::ParseResult<DialogueTopic> parse_dialogue_topic(io::SpanReader& data,
                                                                  const FormContext& ctx);
[[nodiscard]] io::ParseResult<DialogueInfo> parse_dialogue_info(io::SpanReader& data,
                                                                const FormContext& ctx);
[[nodiscard]] io::ParseResult<VoiceType> parse_voice_type(io::SpanReader& data,
                                                          const FormContext& ctx);
[[nodiscard]] io::ParseResult<Relationship> parse_relationship(io::SpanReader& data,
                                                               const FormContext& ctx);

} // namespace bethconv::record
