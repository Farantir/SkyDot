// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/forms_actor.hpp"

#include <optional>
#include <utility>

namespace bethconv::record {
namespace {

/// BOD2 or BODT. BODT is 12 bytes (slots, flags, armor type) except on 9 ARMO
/// where it stops after the flags.
bool read_body_template(BodyTemplate& out, const FieldHeader& field, io::SpanReader& body,
                        std::optional<io::ParseError>& failure) {
    if (field.type == FourCC{"BOD2"}) {
        take(failure, body.get<std::uint32_t>(), out.slots);
        take(failure, body.get<std::uint32_t>(), out.armor_type);
    } else if (field.type == FourCC{"BODT"}) {
        take(failure, body.get<std::uint32_t>(), out.slots);
        take(failure, body.get<std::uint32_t>(), out.flags);
        if (body.remaining() >= 4) {
            take(failure, body.get<std::uint32_t>(), out.armor_type);
        }
    } else {
        return false;
    }
    out.present = true;
    return true;
}

/// MOD<n>/MO<n>T/MO<n>S into `model`, the numbered variants of MODL/MODT/MODS.
bool read_numbered_model(ModelData& model, char n, const FieldHeader& field, io::SpanReader& body,
                         std::optional<io::ParseError>& failure) {
    const char path[5] = {'M', 'O', 'D', n, '\0'};
    const char hashes[5] = {'M', 'O', n, 'T', '\0'};
    const char alternates[5] = {'M', 'O', n, 'S', '\0'};
    const auto tag = [](const char (&s)[5]) {
        return FourCC{static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) |
                      static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 8 |
                      static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 16 |
                      static_cast<std::uint32_t>(static_cast<unsigned char>(s[3])) << 24};
    };
    FieldHeader as_model = field;
    if (field.type == tag(path)) {
        as_model.type = FourCC{"MODL"};
    } else if (field.type == tag(hashes)) {
        as_model.type = FourCC{"MODT"};
    } else if (field.type == tag(alternates)) {
        as_model.type = FourCC{"MODS"};
    } else {
        return false;
    }
    return read_model_field(model, as_model, body, failure);
}

void keep(std::vector<RawField>& other, const FieldHeader& field, io::SpanReader& body,
          std::optional<io::ParseError>& failure) {
    RawField raw{field.type, {}};
    take(failure, read_verbatim(body), raw.bytes);
    if (!failure) {
        other.push_back(std::move(raw));
    }
}

} // namespace

io::ParseResult<Armor> parse_armor(io::SpanReader& data, const FormContext& ctx) {
    Armor out;
    const BaseObjectFields into{.editor_id = &out.editor_id,
                                .bounds = &out.bounds,
                                .name = &out.name,
                                .keywords = &out.keywords,
                                .scripts = &out.scripts};
    const auto walked = walk_fields(
        data, FourCC{"ARMO"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body, std::optional<io::ParseError>& failure) {
            if (read_body_template(out.body, field, body, failure) ||
                read_numbered_model(out.male_model, '2', field, body, failure) ||
                read_numbered_model(out.female_model, '4', field, body, failure)) {
                return true;
            }
            if (field.type == FourCC{"MODL"}) {
                FormId addon;
                take(failure, read_formid(body), addon);
                if (!failure) {
                    out.addons.push_back(addon);
                }
            } else if (field.type == FourCC{"DESC"}) {
                take(failure, read_lstring(body, ctx, FourCC{"ARMO"}, field.type, StringKind::description),
                     out.description);
            } else if (field.type == FourCC{"EITM"}) {
                take(failure, read_formid(body), out.enchantment);
            } else if (field.type == FourCC{"ETYP"}) {
                take(failure, read_formid(body), out.equip_slot);
            } else if (field.type == FourCC{"BIDS"}) {
                take(failure, read_formid(body), out.bash_impact);
            } else if (field.type == FourCC{"BAMT"}) {
                take(failure, read_formid(body), out.bash_material);
            } else if (field.type == FourCC{"YNAM"}) {
                take(failure, read_formid(body), out.pickup_sound);
            } else if (field.type == FourCC{"ZNAM"}) {
                take(failure, read_formid(body), out.drop_sound);
            } else if (field.type == FourCC{"RNAM"}) {
                take(failure, read_formid(body), out.race);
            } else if (field.type == FourCC{"TNAM"}) {
                take(failure, read_formid(body), out.template_armor);
            } else if (field.type == FourCC{"DATA"}) {
                take(failure, body.get<std::uint32_t>(), out.value);
                take(failure, body.get<float>(), out.weight);
            } else if (field.type == FourCC{"DNAM"}) {
                take(failure, body.get<std::uint32_t>(), out.armor_rating);
            } else {
                return read_base_object_field(into, FourCC{"ARMO"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<ArmorAddon> parse_armor_addon(io::SpanReader& data, const FormContext& ctx) {
    ArmorAddon out;
    const BaseObjectFields into{.editor_id = &out.editor_id};
    const auto walked = walk_fields(
        data, FourCC{"ARMA"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body, std::optional<io::ParseError>& failure) {
            if (read_body_template(out.body, field, body, failure) ||
                read_numbered_model(out.male_model, '2', field, body, failure) ||
                read_numbered_model(out.female_model, '3', field, body, failure) ||
                read_numbered_model(out.male_first_person, '4', field, body, failure) ||
                read_numbered_model(out.female_first_person, '5', field, body, failure)) {
                return true;
            }
            if (field.type == FourCC{"RNAM"}) {
                take(failure, read_formid(body), out.race);
            } else if (field.type == FourCC{"DNAM"}) {
                take(failure, body.get<std::uint8_t>(), out.male_priority);
                take(failure, body.get<std::uint8_t>(), out.female_priority);
                take(failure, body.get<std::uint8_t>(), out.male_weight_slider);
                take(failure, body.get<std::uint8_t>(), out.female_weight_slider);
                take(failure, body.get<std::uint16_t>(), out.dnam_unknown);
                take(failure, body.get<std::uint8_t>(), out.detection_sound);
                take(failure, body.get<std::uint8_t>(), out.dnam_unknown2);
                take(failure, body.get<float>(), out.weapon_adjust);
            } else if (field.type == FourCC{"NAM0"}) {
                take(failure, read_formid(body), out.male_skin_texture);
            } else if (field.type == FourCC{"NAM1"}) {
                take(failure, read_formid(body), out.female_skin_texture);
            } else if (field.type == FourCC{"NAM2"}) {
                take(failure, read_formid(body), out.male_skin_swap);
            } else if (field.type == FourCC{"NAM3"}) {
                take(failure, read_formid(body), out.female_skin_swap);
            } else if (field.type == FourCC{"SNDD"}) {
                take(failure, read_formid(body), out.footstep_sound);
            } else if (field.type == FourCC{"ONAM"}) {
                take(failure, read_formid(body), out.art_object);
            } else if (field.type == FourCC{"MODL"}) {
                FormId race;
                take(failure, read_formid(body), race);
                if (!failure) {
                    out.additional_races.push_back(race);
                }
            } else {
                return read_base_object_field(into, FourCC{"ARMA"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Outfit> parse_outfit(io::SpanReader& data, const FormContext& ctx) {
    Outfit out;
    const BaseObjectFields into{.editor_id = &out.editor_id};
    const auto walked = walk_fields(
        data, FourCC{"OTFT"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body, std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"INAM"}) {
                take(failure, read_formid_array(body), out.items);
                return true;
            }
            return read_base_object_field(into, FourCC{"OTFT"}, field, body, ctx, failure);
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<LeveledItem> parse_leveled_item(io::SpanReader& data, const FormContext& ctx) {
    LeveledItem out;
    const BaseObjectFields into{.editor_id = &out.editor_id, .bounds = &out.bounds};
    const auto walked = walk_fields(
        data, FourCC{"LVLI"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body, std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"LVLD"}) {
                take(failure, body.get<std::uint8_t>(), out.chance_none);
            } else if (field.type == FourCC{"LVLF"}) {
                take(failure, body.get<std::uint8_t>(), out.flags);
            } else if (field.type == FourCC{"LVLG"}) {
                take(failure, read_formid(body), out.chance_global);
            } else if (field.type == FourCC{"LLCT"}) {
                take(failure, body.get<std::uint8_t>(), out.declared_count);
            } else if (field.type == FourCC{"LVLO"}) {
                LeveledItem::Entry entry;
                take(failure, body.get<std::uint16_t>(), entry.level);
                take(failure, body.get<std::uint16_t>(), entry.unknown);
                take(failure, read_formid(body), entry.reference);
                take(failure, body.get<std::uint16_t>(), entry.count);
                take(failure, body.get<std::uint16_t>(), entry.unknown2);
                if (!failure) {
                    out.entries.push_back(entry);
                }
            } else {
                return read_base_object_field(into, FourCC{"LVLI"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Race> parse_race(io::SpanReader& data, const FormContext& ctx) {
    Race out;
    const BaseObjectFields into{.editor_id = &out.editor_id, .name = &out.name, .keywords = &out.keywords};
    enum class Section : std::uint8_t { none, head, body, behaviour };
    Section section = Section::none;
    std::size_t sex = 0;
    bool seen_skeleton_sex = false;
    std::size_t voices = 0;
    const auto walked = walk_fields(
        data, FourCC{"RACE"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body, std::optional<io::ParseError>& failure) {
            if (read_body_template(out.body, field, body, failure)) {
                return true;
            }
            const FourCC t = field.type;
            if (t == FourCC{"NAM0"}) {
                section = Section::head;
            } else if (t == FourCC{"NAM1"}) {
                section = Section::body;
            } else if (t == FourCC{"NAM3"}) {
                section = Section::behaviour;
            } else if (t == FourCC{"MNAM"}) {
                sex = 0;
                seen_skeleton_sex = true;
            } else if (t == FourCC{"FNAM"}) {
                sex = 1;
                seen_skeleton_sex = true;
            } else if (t == FourCC{"ANAM"} && seen_skeleton_sex) {
                take(failure, read_zstring(body), out.sexes[sex].skeleton);
            } else if (t == FourCC{"INDX"} && section == Section::body) {
                Race::Sex::BodyPart part;
                take(failure, body.get<std::uint32_t>(), part.index);
                if (!failure) {
                    out.sexes[sex].body_parts.push_back(part);
                }
            } else if (t == FourCC{"MODL"} && section == Section::body &&
                       !out.sexes[sex].body_parts.empty()) {
                take(failure, read_zstring(body), out.sexes[sex].body_parts.back().model);
            } else if (t == FourCC{"MODL"} && section == Section::behaviour) {
                take(failure, read_zstring(body), out.sexes[sex].behaviour);
            } else if (t == FourCC{"HEAD"} && section == Section::head) {
                FormId part;
                take(failure, read_formid(body), part);
                if (!failure) {
                    out.sexes[sex].head_parts.push_back(part);
                }
            } else if (t == FourCC{"DESC"}) {
                take(failure, read_lstring(body, ctx, FourCC{"RACE"}, t, StringKind::description), out.description);
            } else if (t == FourCC{"SPCT"}) {
                take(failure, body.get<std::uint32_t>(), out.declared_spell_count);
            } else if (t == FourCC{"SPLO"}) {
                FormId spell;
                take(failure, read_formid(body), spell);
                if (!failure) {
                    out.spells.push_back(spell);
                }
            } else if (t == FourCC{"WNAM"}) {
                take(failure, read_formid(body), out.skin);
            } else if (t == FourCC{"VTCK"}) {
                take(failure, read_formid(body), out.voice_male);
                take(failure, read_formid(body), out.voice_female);
                ++voices;
            } else if (t == FourCC{"NAM8"}) {
                take(failure, read_formid(body), out.morph_race);
            } else if (t == FourCC{"RNAM"}) {
                take(failure, read_formid(body), out.armor_race);
            } else if (t == FourCC{"DATA"}) {
                take(failure, read_verbatim(body), out.data);
                if (!failure && out.data.size() >= 36) {
                    io::SpanReader d(out.data, "RACE DATA");
                    (void)d.skip(16);
                    take(failure, d.get<float>(), out.height[0]);
                    take(failure, d.get<float>(), out.height[1]);
                    take(failure, d.get<float>(), out.weight[0]);
                    take(failure, d.get<float>(), out.weight[1]);
                    take(failure, d.get<std::uint32_t>(), out.flags);
                }
            } else if (read_base_object_field(into, FourCC{"RACE"}, field, body, ctx, failure)) {
                return true;
            } else {
                // Tints, morphs, attacks, movement, head data not needed yet.
                keep(out.other, field, body, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

} // namespace bethconv::record
