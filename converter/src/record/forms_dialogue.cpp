// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/forms_dialogue.hpp"

#include <optional>
#include <utility>

namespace bethconv::record {

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
