// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/field_reader.hpp"

#include <cstdio>
#include <utility>

namespace bethconv::record {

std::string LString::to_string() const {
    if (is_id) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "#%08X", id);
        return buffer;
    }
    return text;
}

io::ParseResult<std::string> read_zstring(io::SpanReader& body) {
    // An empty field is an empty string (blank MODL occurs in mods).
    if (body.remaining() == 0) {
        return std::string{};
    }
    auto text = body.zstring();
    if (!text) {
        return std::unexpected(std::move(text).error());
    }
    return std::string{*text};
}

io::ParseResult<LString> read_lstring(io::SpanReader& body, const FormContext& ctx,
                                      FourCC record, FourCC field, StringKind kind) {
    if (ctx.localized) {
        auto id = body.get<std::uint32_t>();
        if (!id) {
            return std::unexpected(std::move(id).error());
        }
        // Index 0 means "no string".
        if (*id == 0) {
            return LString{.text = {}, .id = 0, .is_id = false};
        }
        if (ctx.strings != nullptr) {
            const std::string* text = ctx.strings->find(*id, kind);
            if (ctx.tally != nullptr) {
                ctx.tally->localized_string(record, field, *id, text != nullptr);
            }
            if (text != nullptr) {
                return LString{.text = *text, .id = *id, .is_id = false};
            }
        }
        return LString{.text = {}, .id = *id, .is_id = true};
    }
    auto text = read_zstring(body);
    if (!text) {
        return std::unexpected(std::move(text).error());
    }
    return LString{.text = std::move(*text), .id = 0, .is_id = false};
}

io::ParseResult<ObjectBounds> read_object_bounds(io::SpanReader& body) {
    ObjectBounds bounds;
    // Field by field instead of get<ObjectBounds>(), which SpanReader rejects
    // for aggregates on big-endian hosts.
    std::int16_t* const dst[] = {&bounds.x1, &bounds.y1, &bounds.z1,
                                 &bounds.x2, &bounds.y2, &bounds.z2};
    for (std::int16_t* slot : dst) {
        auto value = body.get<std::int16_t>();
        if (!value) {
            return std::unexpected(std::move(value).error());
        }
        *slot = *value;
    }
    return bounds;
}

io::ParseResult<Vec3> read_vec3(io::SpanReader& body) {
    Vec3 v;
    float* const dst[] = {&v.x, &v.y, &v.z};
    for (float* slot : dst) {
        auto value = body.get<float>();
        if (!value) {
            return std::unexpected(std::move(value).error());
        }
        *slot = *value;
    }
    return v;
}

io::ParseResult<FormId> read_formid(io::SpanReader& body) {
    auto value = body.get<std::uint32_t>();
    if (!value) {
        return std::unexpected(std::move(value).error());
    }
    return FormId{*value};
}

io::ParseResult<std::vector<FormId>> read_formid_array(io::SpanReader& body) {
    if (body.remaining() % sizeof(std::uint32_t) != 0) {
        return body.fail(io::ErrorKind::bad_value,
                         "FormID array of " + std::to_string(body.remaining()) +
                             " bytes is not a multiple of 4");
    }
    auto raw = body.array<std::uint32_t>(body.remaining() / sizeof(std::uint32_t));
    if (!raw) {
        return std::unexpected(std::move(raw).error());
    }
    std::vector<FormId> forms;
    forms.reserve(raw->size());
    for (const auto value : *raw) {
        forms.push_back(FormId{value});
    }
    return forms;
}

io::ParseResult<std::vector<std::byte>> read_verbatim(io::SpanReader& body) {
    auto bytes = body.bytes(body.remaining());
    if (!bytes) {
        return std::unexpected(std::move(bytes).error());
    }
    return std::vector<std::byte>(bytes->begin(), bytes->end());
}

// ---- recurring sub-structures ---------------------------------------------

bool read_model_field(ModelData& model, const FieldHeader& field, io::SpanReader& body,
                      std::optional<io::ParseError>& failure) {
    if (field.type == FourCC{"MODL"}) {
        take(failure, read_zstring(body), model.path);
    } else if (field.type == FourCC{"MODT"}) {
        take(failure, read_verbatim(body), model.texture_hashes);
    } else if (field.type == FourCC{"MODS"}) {
        take(failure, read_verbatim(body), model.alternate_textures);
    } else {
        return false;
    }
    return true;
}

bool read_destruction_field(Destruction& dest, const FieldHeader& field, io::SpanReader& body,
                            std::optional<io::ParseError>& failure) {
    if (field.type == FourCC{"DEST"}) {
        take(failure, body.get<std::int32_t>(), dest.health);
        take(failure, body.get<std::uint8_t>(), dest.stage_count);
        take(failure, body.get<std::uint8_t>(), dest.flags);
        take(failure, body.get<std::uint16_t>(), dest.unknown);
    } else if (field.type == FourCC{"DSTD"}) {
        Destruction::Stage stage;
        take(failure, body.get<std::uint8_t>(), stage.health_percent);
        take(failure, body.get<std::uint8_t>(), stage.index);
        take(failure, body.get<std::uint8_t>(), stage.damage_stage);
        take(failure, body.get<std::uint8_t>(), stage.stage_flags);
        take(failure, body.get<std::int32_t>(), stage.self_damage_per_second);
        take(failure, read_formid(body), stage.explosion);
        take(failure, read_formid(body), stage.debris);
        take(failure, body.get<std::int32_t>(), stage.debris_count);
        if (!failure) {
            dest.stages.push_back(std::move(stage));
        }
    } else if (field.type == FourCC{"DMDL"} || field.type == FourCC{"DMDT"} ||
               field.type == FourCC{"DMDS"}) {
        // A model field belongs to the stage opened by DSTD. Without one, it
        // opens its own stage rather than being dropped.
        if (dest.stages.empty()) {
            dest.stages.emplace_back();
        }
        ModelData& model = dest.stages.back().model;
        FieldHeader as_model = field;
        if (field.type == FourCC{"DMDL"}) {
            as_model.type = FourCC{"MODL"};
        } else if (field.type == FourCC{"DMDT"}) {
            as_model.type = FourCC{"MODT"};
        } else {
            as_model.type = FourCC{"MODS"};
        }
        return read_model_field(model, as_model, body, failure);
    } else if (field.type == FourCC{"DSTF"}) {
        // Zero-length terminator (0 bytes on all 793 vanilla ACTI stages).
        // Claimed so it is not counted as unhandled.
    } else {
        return false;
    }
    return true;
}

bool read_keyword_field(KeywordList& keywords, const FieldHeader& field, io::SpanReader& body,
                        std::optional<io::ParseError>& failure) {
    if (field.type == FourCC{"KSIZ"}) {
        take(failure, body.get<std::uint32_t>(), keywords.declared_count);
    } else if (field.type == FourCC{"KWDA"}) {
        auto more = read_formid_array(body);
        if (!more) {
            if (!failure) {
                failure = std::move(more).error();
            }
            return true;
        }
        // KWDA can appear more than once (545 vanilla MISC have a 4-byte one
        // and 28 an 8-byte one), so append.
        keywords.keywords.insert(keywords.keywords.end(), more->begin(), more->end());
    } else {
        return false;
    }
    return true;
}

bool read_container_field(std::vector<ContainerItem>& items, std::uint32_t& declared_count,
                          const FieldHeader& field, io::SpanReader& body,
                          std::optional<io::ParseError>& failure) {
    if (field.type == FourCC{"COCT"}) {
        take(failure, body.get<std::uint32_t>(), declared_count);
    } else if (field.type == FourCC{"CNTO"}) {
        ContainerItem item;
        take(failure, read_formid(body), item.item);
        take(failure, body.get<std::int32_t>(), item.count);
        if (!failure) {
            items.push_back(item);
        }
    } else if (field.type == FourCC{"COED"}) {
        ContainerItem::Extra extra;
        take(failure, read_formid(body), extra.owner);
        take(failure, body.get<std::uint32_t>(), extra.rank_or_global);
        take(failure, body.get<float>(), extra.condition);
        if (!failure) {
            // A COED without a preceding CNTO is kept on an empty entry so the
            // field is still accounted for.
            if (items.empty()) {
                items.emplace_back();
            }
            items.back().extra = extra;
        }
    } else {
        return false;
    }
    return true;
}

bool read_effect_field(std::vector<EffectItem>& effects, const FieldHeader& field,
                       io::SpanReader& body, std::optional<io::ParseError>& failure) {
    if (field.type == FourCC{"EFID"}) {
        EffectItem item;
        take(failure, read_formid(body), item.effect);
        if (!failure) {
            effects.push_back(std::move(item));
        }
    } else if (field.type == FourCC{"EFIT"}) {
        if (effects.empty()) {
            effects.emplace_back();
        }
        EffectItem& item = effects.back();
        take(failure, body.get<float>(), item.magnitude);
        take(failure, body.get<std::uint32_t>(), item.area);
        take(failure, body.get<std::uint32_t>(), item.duration);
    } else if (field.type == FourCC{"CTDA"}) {
        if (effects.empty()) {
            effects.emplace_back();
        }
        std::vector<std::byte> condition;
        take(failure, read_verbatim(body), condition);
        if (!failure) {
            effects.back().conditions.push_back(std::move(condition));
        }
    } else if (field.type == FourCC{"CIS1"} || field.type == FourCC{"CIS2"}) {
        if (effects.empty()) {
            effects.emplace_back();
        }
        std::string text;
        take(failure, read_zstring(body), text);
        if (!failure) {
            effects.back().condition_strings.push_back(std::move(text));
        }
    } else {
        return false;
    }
    return true;
}

bool read_base_object_field(const BaseObjectFields& into, FourCC self, const FieldHeader& field,
                            io::SpanReader& body, const FormContext& ctx,
                            std::optional<io::ParseError>& failure) {
    if (field.type == FourCC{"EDID"} && into.editor_id != nullptr) {
        take(failure, read_zstring(body), *into.editor_id);
        return true;
    }
    if (field.type == FourCC{"OBND"} && into.bounds != nullptr) {
        take(failure, read_object_bounds(body), *into.bounds);
        return true;
    }
    if (field.type == FourCC{"FULL"} && into.name != nullptr) {
        take(failure, read_lstring(body, ctx, self, field.type), *into.name);
        return true;
    }
    if (field.type == FourCC{"VMAD"} && into.scripts != nullptr) {
        take(failure, read_script_data(body), *into.scripts);
        return true;
    }
    if (into.model != nullptr && read_model_field(*into.model, field, body, failure)) {
        return true;
    }
    if (into.keywords != nullptr && read_keyword_field(*into.keywords, field, body, failure)) {
        return true;
    }
    if (into.destruction != nullptr &&
        read_destruction_field(*into.destruction, field, body, failure)) {
        return true;
    }
    return false;
}

io::ParseResult<std::size_t> entry_count(const io::SpanReader& body, std::size_t stride,
                                         std::string_view what) {
    if (stride == 0 || body.remaining() % stride != 0) {
        return body.fail(io::ErrorKind::bad_value,
                         std::string{what} + " of " + std::to_string(body.remaining()) +
                             " bytes is not a multiple of " + std::to_string(stride));
    }
    return body.remaining() / stride;
}

void note_leftover(const FormContext& ctx, FourCC record, const FieldHeader& field,
                   const io::SpanReader& body) {
    if (ctx.tally != nullptr && body.remaining() != 0) {
        ctx.tally->leftover(record, field.type, body.remaining());
    }
}

void note_unhandled(const FormContext& ctx, FourCC record, const FieldHeader& field) {
    if (ctx.tally != nullptr) {
        ctx.tally->unhandled(record, field.type, field.data_size);
    }
}

} // namespace bethconv::record
