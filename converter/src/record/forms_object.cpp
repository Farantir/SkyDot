// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/forms_object.hpp"

#include <optional>
#include <utility>

namespace bethconv::record {
namespace {

/// Read one fixed-width value from a field and append it to a list (FURN
/// markers, IDLM idles, LVLN entries).
template <typename T, typename Read>
void append(std::optional<io::ParseError>& failure, std::vector<T>& into, Read&& read) {
    T value{};
    take(failure, read(), value);
    if (!failure) {
        into.push_back(std::move(value));
    }
}

/// PFPC: four bytes of seasonal yield, on both FLOR and TREE.
void read_seasonal_yield(std::optional<io::ParseError>& failure,
                         std::array<std::uint8_t, 4>& into, io::SpanReader& body) {
    for (std::uint8_t& slot : into) {
        take(failure, body.get<std::uint8_t>(), slot);
    }
}

} // namespace

io::ParseResult<TextureSet> parse_texture_set(io::SpanReader& data, const FormContext& ctx) {
    TextureSet out;
    const auto walked = walk_fields(
        data, FourCC{"TXST"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            // TX00..TX07 differ only in the last character, the slot index.
            const auto tag = field.type.to_string();
            if (tag.size() == 4 && tag[0] == 'T' && tag[1] == 'X' && tag[2] == '0' &&
                tag[3] >= '0' && tag[3] < static_cast<char>('0' + TextureSet::k_slots)) {
                take(failure, read_zstring(body),
                     out.textures[static_cast<std::size_t>(tag[3] - '0')]);
                return true;
            }
            if (field.type == FourCC{"DNAM"}) {
                take(failure, body.get<std::uint16_t>(), out.flags);
            } else if (field.type == FourCC{"DODT"}) {
                take(failure, read_verbatim(body), out.decal);
            } else {
                const BaseObjectFields into{.editor_id = &out.editor_id, .bounds = &out.bounds};
                return read_base_object_field(into, FourCC{"TXST"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Activator> parse_activator(io::SpanReader& data, const FormContext& ctx) {
    Activator out;
    const BaseObjectFields into{.editor_id = &out.editor_id,
                                .bounds = &out.bounds,
                                .model = &out.model,
                                .name = &out.name,
                                .keywords = &out.keywords,
                                .destruction = &out.destruction,
                                .scripts = &out.scripts};
    const auto walked = walk_fields(
        data, FourCC{"ACTI"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"PNAM"}) {
                take(failure, body.get<std::uint32_t>(), out.marker_colour);
            } else if (field.type == FourCC{"SNAM"}) {
                take(failure, read_formid(body), out.looping_sound);
            } else if (field.type == FourCC{"VNAM"}) {
                take(failure, read_formid(body), out.activation_sound);
            } else if (field.type == FourCC{"WNAM"}) {
                take(failure, read_formid(body), out.water_type);
            } else if (field.type == FourCC{"RNAM"}) {
                take(failure, read_lstring(body, ctx, FourCC{"ACTI"}, field.type),
                     out.activate_text);
            } else if (field.type == FourCC{"FNAM"}) {
                take(failure, body.get<std::uint16_t>(), out.flags);
            } else if (field.type == FourCC{"KNAM"}) {
                take(failure, read_formid(body), out.interaction_keyword);
            } else {
                return read_base_object_field(into, FourCC{"ACTI"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Container> parse_container(io::SpanReader& data, const FormContext& ctx) {
    Container out;
    const BaseObjectFields into{.editor_id = &out.editor_id,
                                .bounds = &out.bounds,
                                .model = &out.model,
                                .name = &out.name,
                                .destruction = &out.destruction,
                                .scripts = &out.scripts};
    const auto walked = walk_fields(
        data, FourCC{"CONT"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"DATA"}) {
                take(failure, body.get<std::uint8_t>(), out.flags);
                take(failure, body.get<float>(), out.weight);
            } else if (field.type == FourCC{"SNAM"}) {
                take(failure, read_formid(body), out.open_sound);
            } else if (field.type == FourCC{"QNAM"}) {
                take(failure, read_formid(body), out.close_sound);
            } else if (read_container_field(out.items, out.declared_item_count, field, body,
                                            failure)) {
                return true;
            } else {
                return read_base_object_field(into, FourCC{"CONT"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<MiscItem> parse_misc_item(io::SpanReader& data, const FormContext& ctx) {
    MiscItem out;
    const BaseObjectFields into{.editor_id = &out.editor_id,
                                .bounds = &out.bounds,
                                .model = &out.model,
                                .name = &out.name,
                                .keywords = &out.keywords,
                                .scripts = &out.scripts};
    const auto walked = walk_fields(
        data, FourCC{"MISC"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"DATA"}) {
                take(failure, body.get<std::int32_t>(), out.value);
                take(failure, body.get<float>(), out.weight);
            } else if (field.type == FourCC{"ICON"}) {
                take(failure, read_zstring(body), out.icon);
            } else if (field.type == FourCC{"YNAM"}) {
                take(failure, read_formid(body), out.pickup_sound);
            } else if (field.type == FourCC{"ZNAM"}) {
                take(failure, read_formid(body), out.drop_sound);
            } else {
                return read_base_object_field(into, FourCC{"MISC"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<MovableStatic> parse_movable_static(io::SpanReader& data,
                                                    const FormContext& ctx) {
    MovableStatic out;
    const BaseObjectFields into{.editor_id = &out.editor_id,
                                .bounds = &out.bounds,
                                .model = &out.model,
                                .destruction = &out.destruction};
    const auto walked = walk_fields(
        data, FourCC{"MSTT"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"DATA"}) {
                take(failure, body.get<std::uint8_t>(), out.flags);
            } else if (field.type == FourCC{"SNAM"}) {
                take(failure, read_formid(body), out.ambient_sound);
            } else {
                return read_base_object_field(into, FourCC{"MSTT"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Furniture> parse_furniture(io::SpanReader& data, const FormContext& ctx) {
    Furniture out;
    const BaseObjectFields into{.editor_id = &out.editor_id,
                                .bounds = &out.bounds,
                                .model = &out.model,
                                .name = &out.name,
                                .keywords = &out.keywords,
                                .destruction = &out.destruction,
                                .scripts = &out.scripts};
    const auto walked = walk_fields(
        data, FourCC{"FURN"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"FNAM"}) {
                take(failure, body.get<std::uint16_t>(), out.flags);
            } else if (field.type == FourCC{"KNAM"}) {
                take(failure, read_formid(body), out.interaction_keyword);
            } else if (field.type == FourCC{"MNAM"}) {
                take(failure, body.get<std::uint32_t>(), out.marker_flags);
            } else if (field.type == FourCC{"WBDT"}) {
                take(failure, body.get<std::uint8_t>(), out.bench_type);
                take(failure, body.get<std::uint8_t>(), out.bench_skill);
            } else if (field.type == FourCC{"PNAM"}) {
                take(failure, body.get<std::uint32_t>(), out.pnam);
            } else if (field.type == FourCC{"ENAM"}) {
                append(failure, out.marker_indices, [&] { return body.get<std::int32_t>(); });
            } else if (field.type == FourCC{"NAM0"}) {
                append(failure, out.marker_flags2, [&] { return body.get<std::uint32_t>(); });
            } else if (field.type == FourCC{"FNPR"}) {
                append(failure, out.entry_points, [&] { return body.get<std::uint32_t>(); });
            } else if (field.type == FourCC{"FNMK"}) {
                append(failure, out.marker_keywords, [&] { return read_formid(body); });
            } else if (field.type == FourCC{"XMRK"}) {
                append(failure, out.marker_models, [&] { return read_zstring(body); });
            } else {
                return read_base_object_field(into, FourCC{"FURN"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Flora> parse_flora(io::SpanReader& data, const FormContext& ctx) {
    Flora out;
    const BaseObjectFields into{.editor_id = &out.editor_id,
                                .bounds = &out.bounds,
                                .model = &out.model,
                                .name = &out.name,
                                .keywords = &out.keywords,
                                .scripts = &out.scripts};
    const auto walked = walk_fields(
        data, FourCC{"FLOR"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"PNAM"}) {
                take(failure, body.get<std::uint32_t>(), out.pnam);
            } else if (field.type == FourCC{"RNAM"}) {
                take(failure, read_lstring(body, ctx, FourCC{"FLOR"}, field.type),
                     out.activate_text);
            } else if (field.type == FourCC{"FNAM"}) {
                take(failure, body.get<std::uint16_t>(), out.flags);
            } else if (field.type == FourCC{"SNAM"}) {
                take(failure, read_formid(body), out.harvest_sound);
            } else if (field.type == FourCC{"PFIG"}) {
                take(failure, read_formid(body), out.ingredient);
            } else if (field.type == FourCC{"PFPC"}) {
                read_seasonal_yield(failure, out.seasonal_yield, body);
            } else {
                return read_base_object_field(into, FourCC{"FLOR"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Tree> parse_tree(io::SpanReader& data, const FormContext& ctx) {
    Tree out;
    const BaseObjectFields into{.editor_id = &out.editor_id,
                                .bounds = &out.bounds,
                                .model = &out.model,
                                .name = &out.name,
                                .scripts = &out.scripts};
    const auto walked = walk_fields(
        data, FourCC{"TREE"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"PFIG"}) {
                take(failure, read_formid(body), out.ingredient);
            } else if (field.type == FourCC{"SNAM"}) {
                take(failure, read_formid(body), out.harvest_sound);
            } else if (field.type == FourCC{"PFPC"}) {
                read_seasonal_yield(failure, out.seasonal_yield, body);
            } else if (field.type == FourCC{"CNAM"}) {
                take(failure, body.get<float>(), out.trunk_flexibility);
                take(failure, body.get<float>(), out.branch_flexibility);
                if (!failure && body.remaining() != 0) {
                    take(failure, read_verbatim(body), out.cnam_rest);
                }
            } else {
                return read_base_object_field(into, FourCC{"TREE"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Key> parse_key(io::SpanReader& data, const FormContext& ctx) {
    Key out;
    const BaseObjectFields into{.editor_id = &out.editor_id,
                                .bounds = &out.bounds,
                                .model = &out.model,
                                .name = &out.name,
                                .keywords = &out.keywords,
                                .scripts = &out.scripts};
    const auto walked = walk_fields(
        data, FourCC{"KEYM"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"DATA"}) {
                take(failure, body.get<std::int32_t>(), out.value);
                take(failure, body.get<float>(), out.weight);
            } else if (field.type == FourCC{"YNAM"}) {
                take(failure, read_formid(body), out.pickup_sound);
            } else if (field.type == FourCC{"ZNAM"}) {
                take(failure, read_formid(body), out.drop_sound);
            } else {
                return read_base_object_field(into, FourCC{"KEYM"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Ingestible> parse_ingestible(io::SpanReader& data, const FormContext& ctx) {
    Ingestible out;
    const BaseObjectFields into{.editor_id = &out.editor_id,
                                .bounds = &out.bounds,
                                .model = &out.model,
                                .name = &out.name,
                                .keywords = &out.keywords};
    const auto walked = walk_fields(
        data, FourCC{"ALCH"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"DATA"}) {
                take(failure, body.get<float>(), out.weight);
            } else if (field.type == FourCC{"ENIT"}) {
                take(failure, body.get<std::int32_t>(), out.value);
                take(failure, body.get<std::uint32_t>(), out.flags);
                take(failure, read_formid(body), out.addiction);
                take(failure, body.get<std::uint32_t>(), out.addiction_chance);
                take(failure, read_formid(body), out.use_sound);
            } else if (field.type == FourCC{"YNAM"}) {
                take(failure, read_formid(body), out.pickup_sound);
            } else if (field.type == FourCC{"ZNAM"}) {
                take(failure, read_formid(body), out.drop_sound);
            } else if (read_effect_field(out.effects, field, body, failure)) {
                return true;
            } else {
                return read_base_object_field(into, FourCC{"ALCH"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Ammo> parse_ammo(io::SpanReader& data, const FormContext& ctx) {
    Ammo out;
    const BaseObjectFields into{.editor_id = &out.editor_id,
                                .bounds = &out.bounds,
                                .model = &out.model,
                                .name = &out.name,
                                .keywords = &out.keywords};
    const auto walked = walk_fields(
        data, FourCC{"AMMO"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"DATA"}) {
                take(failure, read_formid(body), out.projectile);
                take(failure, body.get<std::uint32_t>(), out.flags);
                take(failure, body.get<float>(), out.damage);
                take(failure, body.get<std::int32_t>(), out.value);
                // Weight, added in 1.71: absent in LE, present in all SE records.
                if (!failure && body.remaining() >= sizeof(float)) {
                    float weight{};
                    take(failure, body.get<float>(), weight);
                    if (!failure) {
                        out.weight = weight;
                    }
                }
            } else if (field.type == FourCC{"DESC"}) {
                take(failure,
                     read_lstring(body, ctx, FourCC{"AMMO"}, field.type,
                                  StringKind::description),
                     out.description);
            } else if (field.type == FourCC{"YNAM"}) {
                take(failure, read_formid(body), out.pickup_sound);
            } else if (field.type == FourCC{"ZNAM"}) {
                take(failure, read_formid(body), out.drop_sound);
            } else {
                return read_base_object_field(into, FourCC{"AMMO"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Weapon> parse_weapon(io::SpanReader& data, const FormContext& ctx) {
    Weapon out;
    const BaseObjectFields into{.editor_id = &out.editor_id,
                                .bounds = &out.bounds,
                                .model = &out.model,
                                .name = &out.name,
                                .keywords = &out.keywords,
                                .scripts = &out.scripts};
    const auto walked = walk_fields(
        data, FourCC{"WEAP"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"DATA"}) {
                take(failure, body.get<std::int32_t>(), out.value);
                take(failure, body.get<float>(), out.weight);
                take(failure, body.get<std::int16_t>(), out.damage);
            } else if (field.type == FourCC{"DNAM"}) {
                take(failure, read_verbatim(body), out.weapon_data);
            } else if (field.type == FourCC{"CRDT"}) {
                take(failure, read_verbatim(body), out.critical_data);
            } else if (field.type == FourCC{"DESC"}) {
                take(failure,
                     read_lstring(body, ctx, FourCC{"WEAP"}, field.type,
                                  StringKind::description),
                     out.description);
            } else if (field.type == FourCC{"VNAM"}) {
                take(failure, body.get<std::uint32_t>(), out.detection_sound_level);
            } else if (field.type == FourCC{"CNAM"}) {
                take(failure, read_formid(body), out.template_weapon);
            } else if (field.type == FourCC{"EITM"}) {
                take(failure, read_formid(body), out.enchantment);
            } else if (field.type == FourCC{"EAMT"}) {
                take(failure, body.get<std::uint16_t>(), out.enchantment_amount);
            } else if (field.type == FourCC{"ETYP"}) {
                take(failure, read_formid(body), out.equip_type);
            } else if (field.type == FourCC{"BIDS"}) {
                take(failure, read_formid(body), out.block_bash_impact);
            } else if (field.type == FourCC{"BAMT"}) {
                take(failure, read_formid(body), out.block_material);
            } else if (field.type == FourCC{"INAM"}) {
                take(failure, read_formid(body), out.impact_data_set);
            } else if (field.type == FourCC{"WNAM"}) {
                take(failure, read_formid(body), out.first_person_model);
            } else if (field.type == FourCC{"SNAM"}) {
                take(failure, read_formid(body), out.attack_sound);
            } else if (field.type == FourCC{"XNAM"}) {
                take(failure, read_formid(body), out.attack_sound_2d);
            } else if (field.type == FourCC{"NAM7"}) {
                take(failure, read_formid(body), out.attack_loop_sound);
            } else if (field.type == FourCC{"TNAM"}) {
                take(failure, read_formid(body), out.attack_fail_sound);
            } else if (field.type == FourCC{"UNAM"}) {
                take(failure, read_formid(body), out.idle_sound);
            } else if (field.type == FourCC{"NAM9"}) {
                take(failure, read_formid(body), out.equip_sound);
            } else if (field.type == FourCC{"NAM8"}) {
                take(failure, read_formid(body), out.unequip_sound);
            } else if (field.type == FourCC{"NNAM"}) {
                take(failure, read_zstring(body), out.attach_node);
            } else {
                return read_base_object_field(into, FourCC{"WEAP"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Projectile> parse_projectile(io::SpanReader& data, const FormContext& ctx) {
    Projectile out;
    const BaseObjectFields into{.editor_id = &out.editor_id,
                                .bounds = &out.bounds,
                                .model = &out.model,
                                .name = &out.name,
                                .destruction = &out.destruction};
    const auto walked = walk_fields(
        data, FourCC{"PROJ"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"DATA"}) {
                take(failure, read_verbatim(body), out.data);
            } else if (field.type == FourCC{"NAM1"}) {
                take(failure, read_zstring(body), out.muzzle_flash_model);
            } else if (field.type == FourCC{"NAM2"}) {
                take(failure, read_verbatim(body), out.muzzle_flash_hashes);
            } else if (field.type == FourCC{"VNAM"}) {
                take(failure, body.get<std::uint32_t>(), out.sound_level);
            } else {
                return read_base_object_field(into, FourCC{"PROJ"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<IdleMarker> parse_idle_marker(io::SpanReader& data, const FormContext& ctx) {
    IdleMarker out;
    const BaseObjectFields into{.editor_id = &out.editor_id, .bounds = &out.bounds};
    const auto walked = walk_fields(
        data, FourCC{"IDLM"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"IDLF"}) {
                take(failure, body.get<std::uint8_t>(), out.flags);
            } else if (field.type == FourCC{"IDLC"}) {
                take(failure, body.get<std::uint8_t>(), out.declared_count);
            } else if (field.type == FourCC{"IDLT"}) {
                take(failure, body.get<float>(), out.timer);
            } else if (field.type == FourCC{"IDLA"}) {
                take(failure, read_formid_array(body), out.idles);
            } else {
                return read_base_object_field(into, FourCC{"IDLM"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<LeveledNpc> parse_leveled_npc(io::SpanReader& data, const FormContext& ctx) {
    LeveledNpc out;
    const BaseObjectFields into{
        .editor_id = &out.editor_id, .bounds = &out.bounds, .model = &out.model};
    const auto walked = walk_fields(
        data, FourCC{"LVLN"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"LVLD"}) {
                take(failure, body.get<std::uint8_t>(), out.chance_none);
            } else if (field.type == FourCC{"LVLF"}) {
                take(failure, body.get<std::uint8_t>(), out.flags);
            } else if (field.type == FourCC{"LLCT"}) {
                take(failure, body.get<std::uint8_t>(), out.declared_count);
            } else if (field.type == FourCC{"LVLO"}) {
                LeveledNpc::Entry entry;
                take(failure, body.get<std::uint16_t>(), entry.level);
                take(failure, body.get<std::uint16_t>(), entry.unknown);
                take(failure, read_formid(body), entry.reference);
                take(failure, body.get<std::uint16_t>(), entry.count);
                take(failure, body.get<std::uint16_t>(), entry.unknown2);
                if (!failure) {
                    out.entries.push_back(entry);
                }
            } else if (field.type == FourCC{"COED"}) {
                ContainerItem::Extra extra;
                take(failure, read_formid(body), extra.owner);
                take(failure, body.get<std::uint32_t>(), extra.rank_or_global);
                take(failure, body.get<float>(), extra.condition);
                if (!failure) {
                    out.extra = extra;
                }
            } else {
                return read_base_object_field(into, FourCC{"LVLN"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

} // namespace bethconv::record
