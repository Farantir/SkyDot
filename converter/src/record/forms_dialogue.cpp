// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/forms_dialogue.hpp"

#include <optional>
#include <utility>

namespace bethconv::record {
namespace {

/// A field read for nothing: old-style scripts, VNAM, editor widths.
void skip_rest(std::optional<io::ParseError>& failure, io::SpanReader& body) {
    std::vector<std::byte> skipped;
    take(failure, read_verbatim(body), skipped);
}

} // namespace

io::ParseResult<Scene> parse_scene(io::SpanReader& data, const FormContext& ctx) {
    Scene out;
    bool in_phase = false;
    int phase_stage = 0; // 0 start conditions, 1 completion, 2 after the second NEXT
    bool in_action = false;
    bool timer_seconds_next = false;
    std::uint32_t ignored_count{};
    const auto walked = walk_fields(
        data, FourCC{"SCEN"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            const auto t = field.type;
            if (in_phase) {
                auto& phase = out.phases.back();
                if (read_ctda_field(phase_stage == 0 ? phase.start_conditions
                                                     : phase.completion_conditions,
                                    ignored_count, field, body, failure)) {
                    return true;
                }
            } else if (read_ctda_field(out.conditions, out.declared_condition_count, field, body,
                                       failure)) {
                return true;
            }
            const auto skip = [&] { skip_rest(failure, body); };
            if (t == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (t == FourCC{"VMAD"}) {
                take(failure, read_script_data(body), out.scripts);
                if (!failure && !body.at_end()) {
                    take(failure, read_scene_fragments(body), out.fragments);
                }
            } else if (t == FourCC{"HNAM"}) {
                if (in_phase) {
                    in_phase = false;
                } else {
                    out.phases.emplace_back();
                    in_phase = true;
                    phase_stage = 0;
                }
            } else if (t == FourCC{"ANAM"}) {
                if (in_action) {
                    in_action = false;
                } else {
                    auto& a = out.actions.emplace_back();
                    take(failure, body.get<std::uint16_t>(), a.type);
                    in_action = true;
                    timer_seconds_next = false;
                }
            } else if (t == FourCC{"NEXT"}) {
                if (in_phase) {
                    ++phase_stage;
                } else {
                    skip();
                }
            } else if (t == FourCC{"NAM0"}) {
                if (in_action) {
                    take(failure, read_zstring(body), out.actions.back().name);
                } else if (in_phase) {
                    take(failure, read_zstring(body), out.phases.back().name);
                } else {
                    skip();
                }
            } else if (in_action) {
                auto& a = out.actions.back();
                if (t == FourCC{"ALID"}) {
                    take(failure, body.get<std::int32_t>(), a.alias);
                } else if (t == FourCC{"INAM"}) {
                    take(failure, body.get<std::uint32_t>(), a.index);
                } else if (t == FourCC{"FNAM"}) {
                    take(failure, body.get<std::uint32_t>(), a.flags);
                } else if (t == FourCC{"SNAM"}) {
                    if (timer_seconds_next) {
                        take(failure, body.get<float>(), a.timer_seconds);
                    } else {
                        take(failure, body.get<std::uint32_t>(), a.start_phase);
                        timer_seconds_next = true;
                    }
                } else if (t == FourCC{"ENAM"}) {
                    take(failure, body.get<std::uint32_t>(), a.end_phase);
                } else if (t == FourCC{"DATA"}) {
                    take(failure, read_formid(body), a.topic);
                } else if (t == FourCC{"PNAM"}) {
                    FormId package;
                    take(failure, read_formid(body), package);
                    if (!failure) {
                        a.packages.push_back(package);
                    }
                } else if (t == FourCC{"HTID"}) {
                    take(failure, body.get<std::int32_t>(), a.head_track_alias);
                } else if (t == FourCC{"DMAX"}) {
                    take(failure, body.get<float>(), a.delay_max);
                } else if (t == FourCC{"DMIN"}) {
                    take(failure, body.get<float>(), a.delay_min);
                } else if (t == FourCC{"DEMO"}) {
                    take(failure, body.get<std::uint32_t>(), a.emotion);
                } else if (t == FourCC{"DEVA"}) {
                    take(failure, body.get<std::uint32_t>(), a.emotion_value);
                } else if (t == FourCC{"QNAM"} || t == FourCC{"SCHR"} || t == FourCC{"SCDA"} ||
                           t == FourCC{"SCTX"} || t == FourCC{"SCRO"}) {
                    skip(); // Old-style result script.
                } else {
                    return false;
                }
            } else if (in_phase) {
                if (t == FourCC{"QNAM"} || t == FourCC{"SCHR"} || t == FourCC{"SCDA"} ||
                    t == FourCC{"SCTX"} || t == FourCC{"SCRO"} || t == FourCC{"WNAM"}) {
                    skip(); // Old-style result script and the editor width.
                } else {
                    return false;
                }
            } else if (t == FourCC{"FNAM"}) {
                take(failure, body.get<std::uint32_t>(), out.flags);
            } else if (t == FourCC{"ALID"}) {
                auto& a = out.actors.emplace_back();
                take(failure, body.get<std::int32_t>(), a.alias);
            } else if (t == FourCC{"LNAM"} && !out.actors.empty()) {
                take(failure, body.get<std::uint32_t>(), out.actors.back().flags);
            } else if (t == FourCC{"DNAM"} && !out.actors.empty()) {
                take(failure, body.get<std::uint32_t>(), out.actors.back().behaviour_flags);
            } else if (t == FourCC{"PNAM"}) {
                take(failure, read_formid(body), out.quest);
            } else if (t == FourCC{"INAM"} || t == FourCC{"VNAM"} || t == FourCC{"XNAM"} ||
                       t == FourCC{"SCHR"} || t == FourCC{"SCDA"} || t == FourCC{"SCTX"} ||
                       t == FourCC{"SCRO"} || t == FourCC{"QNAM"} || t == FourCC{"WNAM"}) {
                skip(); // The last action's index, VNAM, old-style scripts.
            } else {
                return false;
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<DialogueBranch> parse_dialogue_branch(io::SpanReader& data,
                                                      const FormContext& ctx) {
    DialogueBranch out;
    const auto walked = walk_fields(
        data, FourCC{"DLBR"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            const auto t = field.type;
            if (t == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (t == FourCC{"QNAM"}) {
                take(failure, read_formid(body), out.quest);
            } else if (t == FourCC{"SNAM"}) {
                take(failure, read_formid(body), out.start_topic);
            } else if (t == FourCC{"DNAM"}) {
                take(failure, body.get<std::uint32_t>(), out.flags);
            } else if (t == FourCC{"TNAM"}) {
                take(failure, body.get<std::uint32_t>(), out.category);
            } else {
                return false;
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<DialogueTopic> parse_dialogue_topic(io::SpanReader& data,
                                                    const FormContext& ctx) {
    DialogueTopic out;
    const auto walked = walk_fields(
        data, FourCC{"DIAL"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            const auto t = field.type;
            if (t == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (t == FourCC{"FULL"}) {
                take(failure, read_lstring(body, ctx, FourCC{"DIAL"}, t), out.text);
            } else if (t == FourCC{"QNAM"}) {
                take(failure, read_formid(body), out.quest);
            } else if (t == FourCC{"BNAM"}) {
                take(failure, read_formid(body), out.branch);
            } else if (t == FourCC{"PNAM"}) {
                take(failure, body.get<float>(), out.priority);
            } else if (t == FourCC{"DATA"}) {
                take(failure, body.get<std::uint8_t>(), out.flags);
                take(failure, body.get<std::uint8_t>(), out.category);
                take(failure, body.get<std::uint16_t>(), out.subtype);
            } else if (t == FourCC{"SNAM"}) {
                std::uint32_t code{};
                take(failure, body.get<std::uint32_t>(), code);
                out.subtype_code = FourCC{code};
            } else if (t == FourCC{"TIFC"}) {
                take(failure, body.get<std::uint32_t>(), out.info_count);
            } else {
                return false;
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<DialogueInfo> parse_dialogue_info(io::SpanReader& data, const FormContext& ctx) {
    DialogueInfo out;
    const auto walked = walk_fields(
        data, FourCC{"INFO"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            const auto t = field.type;
            if (read_ctda_field(out.conditions, out.declared_condition_count, field, body,
                                failure)) {
                return true;
            }
            if (t == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (t == FourCC{"ENAM"}) {
                std::uint16_t flags{};
                take(failure, body.get<std::uint16_t>(), flags);
                take(failure, body.get<std::uint16_t>(), out.reset_hours);
                out.flags = flags;
            } else if (t == FourCC{"CNAM"}) {
                take(failure, body.get<std::uint8_t>(), out.favor_level);
            } else if (t == FourCC{"RNAM"}) {
                // The prompt is in the .STRINGS table, not ILSTRINGS like the
                // responses (all 1,441 in Skyrim.esm resolve there).
                take(failure, read_lstring(body, ctx, FourCC{"INFO"}, t), out.prompt);
            } else if (t == FourCC{"ANAM"}) {
                take(failure, read_formid(body), out.speaker);
            } else if (t == FourCC{"DNAM"}) {
                take(failure, read_formid(body), out.shared_info);
            } else if (t == FourCC{"TCLT"}) {
                FormId link;
                take(failure, read_formid(body), link);
                if (!failure) {
                    out.links.push_back(link);
                }
            } else if (t == FourCC{"TWAT"}) {
                take(failure, read_formid(body), out.walk_away_topic);
            } else if (t == FourCC{"ONAM"}) {
                take(failure, read_formid(body), out.audio_output);
            } else if (t == FourCC{"TRDT"}) {
                auto& r = out.responses.emplace_back();
                take(failure, body.get<std::uint32_t>(), r.emotion);
                take(failure, body.get<std::uint32_t>(), r.emotion_value);
                skip_bytes(failure, body, 4);
                take(failure, body.get<std::uint8_t>(), r.number);
                skip_bytes(failure, body, 3);
                take(failure, read_formid(body), r.sound);
                take(failure, body.get<std::uint8_t>(), r.flags);
                skip_bytes(failure, body, 3);
            } else if (t == FourCC{"NAM1"} && !out.responses.empty()) {
                take(failure, read_lstring(body, ctx, FourCC{"INFO"}, t, StringKind::dialogue),
                     out.responses.back().text);
            } else if (t == FourCC{"NAM2"} && !out.responses.empty()) {
                take(failure, read_zstring(body), out.responses.back().notes);
            } else if (t == FourCC{"VMAD"}) {
                take(failure, read_script_data(body), out.scripts);
                // 231 of 5,540 Skyrim.esm INFO VMADs end after the scripts.
                if (!failure && !body.at_end()) {
                    take(failure, read_info_fragments(body), out.fragments);
                }
            } else if (t == FourCC{"NAM3"} || t == FourCC{"SNAM"} || t == FourCC{"LNAM"} ||
                       t == FourCC{"PNAM"} || t == FourCC{"SCHR"} || t == FourCC{"SCTX"} ||
                       t == FourCC{"NEXT"} || t == FourCC{"QNAM"} || t == FourCC{"DATA"}) {
                // Unused in Skyrim: a response's edit notes, idles, the
                // previous INFO (LE) and the remains of a Fallout result script.
                std::vector<std::byte> skipped;
                take(failure, read_verbatim(body), skipped);
            } else {
                return false;
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<VoiceType> parse_voice_type(io::SpanReader& data, const FormContext& ctx) {
    VoiceType out;
    const auto walked = walk_fields(
        data, FourCC{"VTYP"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"DNAM"}) {
                take(failure, body.get<std::uint8_t>(), out.flags);
            } else {
                return false;
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Relationship> parse_relationship(io::SpanReader& data, const FormContext& ctx) {
    Relationship out;
    const auto walked = walk_fields(
        data, FourCC{"RELA"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"DATA"}) {
                take(failure, read_formid(body), out.parent);
                take(failure, read_formid(body), out.child);
                take(failure, body.get<std::uint16_t>(), out.rank);
                skip_bytes(failure, body, 2);
                take(failure, read_formid(body), out.association);
            } else {
                return false;
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

} // namespace bethconv::record
