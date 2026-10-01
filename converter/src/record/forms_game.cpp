// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/forms_game.hpp"

#include <optional>
#include <utility>

namespace bethconv::record {
namespace {

/// Read a fixed-stride array field into a list; a payload that is not a whole
/// number of entries is an error. (Same helper as in forms_world.cpp.)
template <typename T, typename Read>
void read_entries(std::optional<io::ParseError>& failure, std::vector<T>& into,
                  io::SpanReader& body, std::size_t stride, std::string_view what,
                  Read&& read_one) {
    auto count = entry_count(body, stride, what);
    if (!count) {
        if (!failure) {
            failure = std::move(count).error();
        }
        return;
    }
    for (std::size_t i = 0; i < *count && !failure; ++i) {
        T entry{};
        read_one(entry);
        if (!failure) {
            into.push_back(std::move(entry));
        }
    }
}

/// A field holding one FormID, appended to a list (SPLO, PKID, PNAM, CSDI:
/// one reference per field, repeated). Not to be confused with FormID arrays.
void append_formid(std::optional<io::ParseError>& failure, std::vector<FormId>& into,
                   io::SpanReader& body) {
    FormId id;
    take(failure, read_formid(body), id);
    if (!failure) {
        into.push_back(id);
    }
}

/// CTDA/CIS1/CIS2 directly on a record (FACT), not inside an effect item.
[[nodiscard]] bool read_condition_field(std::vector<std::vector<std::byte>>& conditions,
                                        std::vector<std::string>& strings,
                                        const FieldHeader& field, io::SpanReader& body,
                                        std::optional<io::ParseError>& failure) {
    if (field.type == FourCC{"CTDA"}) {
        std::vector<std::byte> bytes;
        take(failure, read_verbatim(body), bytes);
        if (!failure) {
            conditions.push_back(std::move(bytes));
        }
        return true;
    }
    if (field.type == FourCC{"CIS1"} || field.type == FourCC{"CIS2"}) {
        std::string text;
        take(failure, read_zstring(body), text);
        if (!failure) {
            strings.push_back(std::move(text));
        }
        return true;
    }
    return false;
}

} // namespace

// ---- GMST -----------------------------------------------------------------

GameSetting::Kind GameSetting::kind_of(std::string_view editor_id) noexcept {
    // Only the first character, lowercase as Bethesda writes it. Anything else
    // is `unknown`; `raw` is kept either way.
    if (editor_id.empty()) {
        return Kind::unknown;
    }
    switch (editor_id.front()) {
    case 'b':
        return Kind::boolean;
    case 'i':
        return Kind::integer;
    case 'f':
        return Kind::floating;
    case 's':
        return Kind::string;
    case 'u':
        return Kind::unsigned_int;
    default:
        return Kind::unknown;
    }
}

io::ParseResult<GameSetting> parse_game_setting(io::SpanReader& data, const FormContext& ctx) {
    GameSetting out;
    const auto walked = walk_fields(
        data, FourCC{"GMST"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"DATA"}) {
                // Interpreted after the walk, since EDID decides the type and
                // may come later.
                take(failure, read_verbatim(body), out.raw);
            } else {
                return false;
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }

    out.kind = GameSetting::kind_of(out.editor_id);
    if (out.raw.empty()) {
        return out;
    }

    io::SpanReader value{out.raw, data.origin()};
    switch (out.kind) {
    case GameSetting::Kind::boolean: {
        auto bits = value.get<std::uint32_t>();
        if (!bits) {
            return std::unexpected(std::move(bits).error());
        }
        out.boolean_value = *bits != 0;
        break;
    }
    case GameSetting::Kind::integer: {
        auto bits = value.get<std::int32_t>();
        if (!bits) {
            return std::unexpected(std::move(bits).error());
        }
        out.integer_value = *bits;
        break;
    }
    case GameSetting::Kind::unsigned_int: {
        auto bits = value.get<std::uint32_t>();
        if (!bits) {
            return std::unexpected(std::move(bits).error());
        }
        out.unsigned_value = *bits;
        break;
    }
    case GameSetting::Kind::floating: {
        auto bits = value.get<float>();
        if (!bits) {
            return std::unexpected(std::move(bits).error());
        }
        out.float_value = *bits;
        break;
    }
    case GameSetting::Kind::string: {
        // Index or inline text depending on the plugin, as for FULL.
        auto text = read_lstring(value, ctx, FourCC{"GMST"}, FourCC{"DATA"});
        if (!text) {
            return std::unexpected(std::move(text).error());
        }
        out.text = std::move(*text);
        break;
    }
    case GameSetting::Kind::unknown:
        break;
    }
    return out;
}

// ---- GLOB -----------------------------------------------------------------

io::ParseResult<GlobalVariable> parse_global(io::SpanReader& data, const FormContext& ctx) {
    GlobalVariable out;
    const auto walked = walk_fields(
        data, FourCC{"GLOB"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"FNAM"}) {
                std::uint8_t kind{};
                take(failure, body.get<std::uint8_t>(), kind);
                out.kind = static_cast<char>(kind);
            } else if (field.type == FourCC{"FLTV"}) {
                take(failure, body.get<float>(), out.value);
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

// ---- CLAS -----------------------------------------------------------------

io::ParseResult<ActorClass> parse_actor_class(io::SpanReader& data, const FormContext& ctx) {
    ActorClass out;
    const auto walked = walk_fields(
        data, FourCC{"CLAS"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"FULL"}) {
                take(failure, read_lstring(body, ctx, FourCC{"CLAS"}, field.type), out.name);
            } else if (field.type == FourCC{"DESC"}) {
                take(failure,
                     read_lstring(body, ctx, FourCC{"CLAS"}, field.type,
                                  StringKind::description),
                     out.description);
            } else if (field.type == FourCC{"DATA"}) {
                take(failure, body.get<std::uint32_t>(), out.unknown);
                take(failure, body.get<std::uint8_t>(), out.training_skill);
                take(failure, body.get<std::uint8_t>(), out.training_level);
                for (std::uint8_t& weight : out.skill_weights) {
                    take(failure, body.get<std::uint8_t>(), weight);
                }
                take(failure, body.get<float>(), out.bleedout_default);
                take(failure, body.get<std::uint32_t>(), out.voice_points);
                take(failure, body.get<std::uint8_t>(), out.health_weight);
                take(failure, body.get<std::uint8_t>(), out.magicka_weight);
                take(failure, body.get<std::uint8_t>(), out.stamina_weight);
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

// ---- FACT -----------------------------------------------------------------

io::ParseResult<Faction> parse_faction(io::SpanReader& data, const FormContext& ctx) {
    Faction out;
    const auto walked = walk_fields(
        data, FourCC{"FACT"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"FULL"}) {
                take(failure, read_lstring(body, ctx, FourCC{"FACT"}, field.type), out.name);
            } else if (field.type == FourCC{"XNAM"}) {
                read_entries(failure, out.relations, body, Faction::k_relation_size,
                             "FACT XNAM", [&](Faction::Relation& relation) {
                                 take(failure, read_formid(body), relation.faction);
                                 take(failure, body.get<std::int32_t>(), relation.modifier);
                                 take(failure, body.get<std::uint32_t>(),
                                      relation.combat_reaction);
                             });
            } else if (field.type == FourCC{"DATA"}) {
                take(failure, body.get<std::uint32_t>(), out.flags);
            } else if (field.type == FourCC{"JAIL"}) {
                take(failure, read_formid(body), out.prison);
            } else if (field.type == FourCC{"WAIT"}) {
                take(failure, read_formid(body), out.follower_wait);
            } else if (field.type == FourCC{"STOL"}) {
                take(failure, read_formid(body), out.evidence_chest);
            } else if (field.type == FourCC{"PLCN"}) {
                take(failure, read_formid(body), out.belongings);
            } else if (field.type == FourCC{"CRGR"}) {
                take(failure, read_formid(body), out.crime_group);
            } else if (field.type == FourCC{"JOUT"}) {
                take(failure, read_formid(body), out.jail_outfit);
            } else if (field.type == FourCC{"CRVA"}) {
                take(failure, read_verbatim(body), out.crime_values);
            } else if (field.type == FourCC{"RNAM"}) {
                // RNAM opens a rank; the following MNAM/FNAM are its titles.
                Faction::Rank rank;
                take(failure, body.get<std::uint32_t>(), rank.number);
                if (!failure) {
                    out.ranks.push_back(std::move(rank));
                }
            } else if (field.type == FourCC{"MNAM"} || field.type == FourCC{"FNAM"}) {
                if (out.ranks.empty()) {
                    out.ranks.emplace_back();
                }
                Faction::Rank& rank = out.ranks.back();
                take(failure, read_lstring(body, ctx, FourCC{"FACT"}, field.type),
                     field.type == FourCC{"MNAM"} ? rank.male_title : rank.female_title);
            } else if (field.type == FourCC{"VEND"}) {
                take(failure, read_formid(body), out.vendor_list);
            } else if (field.type == FourCC{"VENC"}) {
                take(failure, read_formid(body), out.vendor_chest);
            } else if (field.type == FourCC{"VENV"}) {
                take(failure, read_verbatim(body), out.vendor_values);
            } else if (field.type == FourCC{"PLVD"}) {
                take(failure, read_verbatim(body), out.vendor_location);
            } else if (field.type == FourCC{"CITC"}) {
                take(failure, body.get<std::uint32_t>(), out.declared_condition_count);
            } else if (read_condition_field(out.conditions, out.condition_strings, field, body,
                                            failure)) {
                return true;
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

// ---- ENCH -----------------------------------------------------------------

io::ParseResult<Enchantment> parse_enchantment(io::SpanReader& data, const FormContext& ctx) {
    Enchantment out;
    const BaseObjectFields into{
        .editor_id = &out.editor_id, .bounds = &out.bounds, .name = &out.name};
    const auto walked = walk_fields(
        data, FourCC{"ENCH"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"ENIT"}) {
                take(failure, body.get<std::int32_t>(), out.cost);
                take(failure, body.get<std::uint32_t>(), out.flags);
                take(failure, body.get<std::uint32_t>(), out.cast_type);
                take(failure, body.get<std::int32_t>(), out.amount);
                take(failure, body.get<std::uint32_t>(), out.target_type);
                take(failure, body.get<std::uint32_t>(), out.enchantment_type);
                take(failure, body.get<float>(), out.charge_time);
                take(failure, read_formid(body), out.base_enchantment);
                // The four pre-1.70 records end here; see `worn_restrictions`.
                if (!failure && body.remaining() >= sizeof(std::uint32_t)) {
                    FormId worn;
                    take(failure, read_formid(body), worn);
                    if (!failure) {
                        out.worn_restrictions = worn;
                    }
                }
            } else if (read_effect_field(out.effects, field, body, failure)) {
                return true;
            } else {
                return read_base_object_field(into, FourCC{"ENCH"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

// ---- SPEL -----------------------------------------------------------------

io::ParseResult<Spell> parse_spell(io::SpanReader& data, const FormContext& ctx) {
    Spell out;
    const BaseObjectFields into{
        .editor_id = &out.editor_id, .bounds = &out.bounds, .name = &out.name};
    const auto walked = walk_fields(
        data, FourCC{"SPEL"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"DESC"}) {
                take(failure,
                     read_lstring(body, ctx, FourCC{"SPEL"}, field.type,
                                  StringKind::description),
                     out.description);
            } else if (field.type == FourCC{"MDOB"}) {
                take(failure, read_formid(body), out.menu_display_object);
            } else if (field.type == FourCC{"ETYP"}) {
                take(failure, read_formid(body), out.equip_type);
            } else if (field.type == FourCC{"SPIT"}) {
                take(failure, body.get<std::int32_t>(), out.cost);
                take(failure, body.get<std::uint32_t>(), out.flags);
                take(failure, body.get<std::uint32_t>(), out.type);
                take(failure, body.get<float>(), out.charge_time);
                take(failure, body.get<std::uint32_t>(), out.cast_type);
                take(failure, body.get<std::uint32_t>(), out.target_type);
                take(failure, body.get<float>(), out.cast_duration);
                take(failure, body.get<float>(), out.range);
                take(failure, read_formid(body), out.half_cost_perk);
            } else if (read_effect_field(out.effects, field, body, failure)) {
                return true;
            } else {
                return read_base_object_field(into, FourCC{"SPEL"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

// ---- NPC_ -----------------------------------------------------------------

namespace {

/// The nineteen NPC_ fields that are a single FormID. A table instead of
/// nineteen near-identical branches.
struct NpcFormIdField {
    FourCC tag;
    FormId Npc::*member;
};

constexpr NpcFormIdField k_npc_formids[] = {
    {FourCC{"INAM"}, &Npc::death_item},        {FourCC{"VTCK"}, &Npc::voice_type},
    {FourCC{"TPLT"}, &Npc::npc_template},      {FourCC{"RNAM"}, &Npc::race},
    {FourCC{"CNAM"}, &Npc::actor_class},       {FourCC{"WNAM"}, &Npc::worn_armor},
    {FourCC{"ANAM"}, &Npc::far_away_model},    {FourCC{"ATKR"}, &Npc::attack_race},
    {FourCC{"HCLF"}, &Npc::hair_colour},       {FourCC{"ZNAM"}, &Npc::combat_style},
    {FourCC{"GNAM"}, &Npc::gift_filter},       {FourCC{"CSCR"}, &Npc::audio_template},
    {FourCC{"DOFT"}, &Npc::default_outfit},    {FourCC{"SOFT"}, &Npc::sleeping_outfit},
    {FourCC{"DPLT"}, &Npc::default_package_list},
    {FourCC{"CRIF"}, &Npc::crime_faction},     {FourCC{"FTST"}, &Npc::head_texture},
    {FourCC{"SPOR"}, &Npc::sleeping_override}, {FourCC{"ECOR"}, &Npc::combat_override},
};

} // namespace

io::ParseResult<Npc> parse_npc(io::SpanReader& data, const FormContext& ctx) {
    Npc out;
    const BaseObjectFields into{.editor_id = &out.editor_id,
                                .bounds = &out.bounds,
                                .name = &out.name,
                                .keywords = &out.keywords,
                                .destruction = &out.destruction,
                                .scripts = &out.scripts};
    const auto walked = walk_fields(
        data, FourCC{"NPC_"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            for (const auto& row : k_npc_formids) {
                if (field.type == row.tag) {
                    take(failure, read_formid(body), out.*row.member);
                    return true;
                }
            }
            if (field.type == FourCC{"SHRT"}) {
                take(failure, read_lstring(body, ctx, FourCC{"NPC_"}, field.type),
                     out.short_name);
            } else if (field.type == FourCC{"ACBS"}) {
                take(failure, body.get<std::uint32_t>(), out.flags);
                take(failure, body.get<std::uint16_t>(), out.magicka_offset);
                take(failure, body.get<std::uint16_t>(), out.stamina_offset);
                take(failure, body.get<std::uint16_t>(), out.level);
                take(failure, body.get<std::uint16_t>(), out.calc_min_level);
                take(failure, body.get<std::uint16_t>(), out.calc_max_level);
                take(failure, body.get<std::uint16_t>(), out.speed_multiplier);
                take(failure, body.get<std::uint16_t>(), out.disposition_base);
                take(failure, body.get<std::uint16_t>(), out.template_flags);
                take(failure, body.get<std::uint16_t>(), out.health_offset);
                take(failure, body.get<std::uint16_t>(), out.bleedout_override);
            } else if (field.type == FourCC{"AIDT"}) {
                take(failure, body.get<std::uint8_t>(), out.aggression);
                take(failure, body.get<std::uint8_t>(), out.confidence);
                take(failure, body.get<std::uint8_t>(), out.energy);
                take(failure, body.get<std::uint8_t>(), out.morality);
                take(failure, body.get<std::uint8_t>(), out.mood);
                take(failure, body.get<std::uint8_t>(), out.assistance);
                take(failure, body.get<std::uint8_t>(), out.aggro_radius_behaviour);
                take(failure, body.get<std::uint8_t>(), out.aidt_unknown);
                take(failure, body.get<std::uint32_t>(), out.warn);
                take(failure, body.get<std::uint32_t>(), out.warn_or_attack);
                take(failure, body.get<std::uint32_t>(), out.attack);
            } else if (field.type == FourCC{"DNAM"}) {
                for (std::uint8_t& value : out.skill_values) {
                    take(failure, body.get<std::uint8_t>(), value);
                }
                for (std::uint8_t& offset : out.skill_offsets) {
                    take(failure, body.get<std::uint8_t>(), offset);
                }
                take(failure, body.get<std::uint16_t>(), out.health);
                take(failure, body.get<std::uint16_t>(), out.magicka);
                take(failure, body.get<std::uint16_t>(), out.stamina);
                take(failure, body.get<std::uint16_t>(), out.dnam_unknown);
                take(failure, body.get<float>(), out.far_away_model_distance);
                take(failure, body.get<std::uint8_t>(), out.geared_up_weapons);
                if (!failure && body.remaining() != 0) {
                    take(failure, read_verbatim(body), out.dnam_tail);
                }
            } else if (field.type == FourCC{"SNAM"}) {
                read_entries(failure, out.factions, body, Npc::k_faction_size, "NPC_ SNAM",
                             [&](Npc::FactionRank& entry) {
                                 take(failure, read_formid(body), entry.faction);
                                 take(failure, body.get<std::int32_t>(), entry.rank);
                             });
            } else if (field.type == FourCC{"PRKZ"}) {
                take(failure, body.get<std::uint32_t>(), out.declared_perk_count);
            } else if (field.type == FourCC{"PRKR"}) {
                read_entries(failure, out.perks, body, Npc::k_perk_size, "NPC_ PRKR",
                             [&](Npc::PerkRank& entry) {
                                 take(failure, read_formid(body), entry.perk);
                                 take(failure, body.get<std::uint32_t>(), entry.rank);
                             });
            } else if (field.type == FourCC{"SPCT"}) {
                take(failure, body.get<std::uint32_t>(), out.declared_spell_count);
            } else if (field.type == FourCC{"SPLO"}) {
                append_formid(failure, out.spells, body);
            } else if (field.type == FourCC{"PKID"}) {
                append_formid(failure, out.packages, body);
            } else if (field.type == FourCC{"PNAM"}) {
                append_formid(failure, out.head_parts, body);
            } else if (field.type == FourCC{"NAM6"}) {
                take(failure, body.get<float>(), out.height);
            } else if (field.type == FourCC{"NAM7"}) {
                take(failure, body.get<float>(), out.weight);
            } else if (field.type == FourCC{"NAM8"}) {
                take(failure, body.get<std::uint32_t>(), out.sound_level);
            } else if (field.type == FourCC{"NAM5"}) {
                take(failure, body.get<std::uint16_t>(), out.nam5);
            } else if (field.type == FourCC{"QNAM"}) {
                take(failure, body.get<float>(), out.text_red);
                take(failure, body.get<float>(), out.text_green);
                take(failure, body.get<float>(), out.text_blue);
            } else if (field.type == FourCC{"NAM9"}) {
                take(failure, read_verbatim(body), out.face_morph);
            } else if (field.type == FourCC{"NAMA"}) {
                take(failure, read_verbatim(body), out.face_parts);
            } else if (field.type == FourCC{"ATKD"}) {
                // ATKD opens an attack; the following ATKE names its animation.
                Npc::Attack attack;
                take(failure, read_verbatim(body), attack.data);
                if (!failure) {
                    out.attacks.push_back(std::move(attack));
                }
            } else if (field.type == FourCC{"ATKE"}) {
                if (out.attacks.empty()) {
                    out.attacks.emplace_back();
                }
                take(failure, read_zstring(body), out.attacks.back().event);
            } else if (field.type == FourCC{"TINI"}) {
                Npc::TintLayer layer;
                take(failure, body.get<std::uint16_t>(), layer.index);
                if (!failure) {
                    out.tint_layers.push_back(layer);
                }
            } else if (field.type == FourCC{"TINC"} || field.type == FourCC{"TINV"} ||
                       field.type == FourCC{"TIAS"}) {
                if (out.tint_layers.empty()) {
                    out.tint_layers.emplace_back();
                }
                Npc::TintLayer& layer = out.tint_layers.back();
                if (field.type == FourCC{"TINC"}) {
                    take(failure, body.get<std::uint32_t>(), layer.colour);
                } else if (field.type == FourCC{"TINV"}) {
                    take(failure, body.get<std::uint32_t>(), layer.value);
                } else {
                    take(failure, body.get<std::int16_t>(), layer.preset);
                }
            } else if (field.type == FourCC{"CSDT"}) {
                Npc::SoundSet set;
                take(failure, body.get<std::uint32_t>(), set.type);
                if (!failure) {
                    out.sounds.push_back(std::move(set));
                }
            } else if (field.type == FourCC{"CSDI"} || field.type == FourCC{"CSDC"}) {
                if (out.sounds.empty()) {
                    out.sounds.emplace_back();
                }
                Npc::SoundSet& set = out.sounds.back();
                if (field.type == FourCC{"CSDI"}) {
                    append_formid(failure, set.sounds, body);
                } else {
                    std::uint8_t chance{};
                    take(failure, body.get<std::uint8_t>(), chance);
                    if (!failure) {
                        set.chances.push_back(chance);
                    }
                }
            } else if (field.type == FourCC{"DATA"}) {
                // 0 bytes on every vanilla record: a marker.
                out.has_data_marker = true;
            } else if (read_container_field(out.items, out.declared_item_count, field, body,
                                            failure)) {
                return true;
            } else {
                return read_base_object_field(into, FourCC{"NPC_"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

// ---- QUST -----------------------------------------------------------------

namespace {

/// The single-FormID fields of an alias.
struct AliasFormIdField {
    FourCC tag;
    FormId Quest::Alias::*member;
};

constexpr AliasFormIdField k_alias_formids[] = {
    {FourCC{"ALFL"}, &Quest::Alias::specific_location},
    {FourCC{"ALFR"}, &Quest::Alias::forced_ref},
    {FourCC{"ALUA"}, &Quest::Alias::unique_actor},
    {FourCC{"ALCO"}, &Quest::Alias::created_object},
    {FourCC{"ALEQ"}, &Quest::Alias::external_quest},
    {FourCC{"ALRT"}, &Quest::Alias::location_ref_type},
    {FourCC{"KNAM"}, &Quest::Alias::keyword},
    {FourCC{"ALDN"}, &Quest::Alias::display_name},
    {FourCC{"VTCK"}, &Quest::Alias::voice_types},
    {FourCC{"SPOR"}, &Quest::Alias::sleep_override},
    {FourCC{"OCOR"}, &Quest::Alias::observe_corpse_override},
    {FourCC{"GWOR"}, &Quest::Alias::guard_warn_override},
    {FourCC{"ECOR"}, &Quest::Alias::combat_override},
};

struct AliasIntField {
    FourCC tag;
    std::int32_t Quest::Alias::*member;
};

constexpr AliasIntField k_alias_ints[] = {
    {FourCC{"ALFI"}, &Quest::Alias::force_into},
    {FourCC{"ALNA"}, &Quest::Alias::near_alias},
    {FourCC{"ALEA"}, &Quest::Alias::external_alias},
    {FourCC{"ALFA"}, &Quest::Alias::location_alias},
};

/// A field inside an open alias. False if it is not an alias field.
bool read_alias_field(Quest::Alias& alias, const FieldHeader& field, io::SpanReader& body,
                      std::optional<io::ParseError>& failure) {
    for (const auto& row : k_alias_formids) {
        if (field.type == row.tag) {
            take(failure, read_formid(body), alias.*row.member);
            return true;
        }
    }
    for (const auto& row : k_alias_ints) {
        if (field.type == row.tag) {
            take(failure, body.get<std::int32_t>(), alias.*row.member);
            return true;
        }
    }
    if (field.type == FourCC{"ALID"}) {
        take(failure, read_zstring(body), alias.name);
    } else if (field.type == FourCC{"FNAM"}) {
        take(failure, body.get<std::uint32_t>(), alias.flags);
    } else if (field.type == FourCC{"ALNT"}) {
        take(failure, body.get<std::uint32_t>(), alias.near_type);
    } else if (field.type == FourCC{"ALCA"}) {
        take(failure, body.get<std::uint32_t>(), alias.create_at);
    } else if (field.type == FourCC{"ALCL"}) {
        take(failure, body.get<std::uint32_t>(), alias.create_level);
    } else if (field.type == FourCC{"ALFD"}) {
        take(failure, body.get<std::uint32_t>(), alias.event_data);
    } else if (field.type == FourCC{"ALFE"}) {
        std::uint32_t event{};
        take(failure, body.get<std::uint32_t>(), event);
        alias.from_event = FourCC{event};
    } else if (field.type == FourCC{"ALSP"}) {
        append_formid(failure, alias.spells, body);
    } else if (field.type == FourCC{"ALFC"}) {
        append_formid(failure, alias.factions, body);
    } else if (field.type == FourCC{"ALPC"}) {
        append_formid(failure, alias.packages, body);
    } else if (read_condition_field(alias.conditions.raw, alias.conditions.strings, field, body,
                                    failure) ||
               read_keyword_field(alias.keywords, field, body, failure) ||
               read_container_field(alias.items, alias.declared_item_count, field, body,
                                    failure)) {
        return true;
    } else {
        return false;
    }
    return true;
}

} // namespace

io::ParseResult<Quest> parse_quest(io::SpanReader& data, const FormContext& ctx) {
    Quest out;
    // Which positional block the next fields belong to.
    enum class Open { header, events, stage, log, objective, target, alias };
    Open open = Open::header;
    auto conditions = [&]() -> Quest::Conditions* {
        switch (open) {
        case Open::header:
            return &out.dialogue_conditions;
        case Open::events:
            return &out.event_conditions;
        case Open::log:
            return &out.stages.back().log.back().conditions;
        case Open::target:
            return &out.objectives.back().targets.back().conditions;
        case Open::alias:
            return &out.aliases.back().conditions;
        case Open::stage:
        case Open::objective:
            break;
        }
        return nullptr;
    };
    const auto walked = walk_fields(
        data, FourCC{"QUST"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            const auto t = field.type;
            if (open == Open::alias) {
                if (t == FourCC{"ALED"}) {
                    open = Open::header;
                    return true;
                }
                return read_alias_field(out.aliases.back(), field, body, failure);
            }
            if (t == FourCC{"ALST"} || t == FourCC{"ALLS"}) {
                auto& alias = out.aliases.emplace_back();
                alias.location = t == FourCC{"ALLS"};
                take(failure, body.get<std::uint32_t>(), alias.id);
                open = Open::alias;
            } else if (t == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (t == FourCC{"FULL"}) {
                take(failure, read_lstring(body, ctx, FourCC{"QUST"}, t), out.name);
            } else if (t == FourCC{"VMAD"}) {
                take(failure, read_script_data(body), out.scripts);
                // 146 of 2,391 SE quests end their VMAD after the scripts.
                if (!failure && !body.at_end()) {
                    take(failure, read_quest_fragments(body, out.scripts), out.fragments);
                }
            } else if (t == FourCC{"DNAM"}) {
                take(failure, body.get<std::uint16_t>(), out.flags);
                take(failure, body.get<std::uint8_t>(), out.priority);
                take(failure, body.get<std::uint8_t>(), out.form_version);
                take(failure, body.get<std::uint32_t>(), out.dnam_unknown);
                take(failure, body.get<std::uint32_t>(), out.type);
            } else if (t == FourCC{"ENAM"}) {
                std::uint32_t event{};
                take(failure, body.get<std::uint32_t>(), event);
                out.event = FourCC{event};
            } else if (t == FourCC{"QTGL"}) {
                append_formid(failure, out.text_globals, body);
            } else if (t == FourCC{"FLTR"}) {
                take(failure, read_zstring(body), out.filter);
            } else if (t == FourCC{"ANAM"}) {
                take(failure, body.get<std::uint32_t>(), out.next_alias_id);
            } else if (t == FourCC{"NEXT"}) {
                out.has_next = true;
                open = Open::events;
            } else if (t == FourCC{"INDX"}) {
                auto& stage = out.stages.emplace_back();
                take(failure, body.get<std::uint16_t>(), stage.index);
                take(failure, body.get<std::uint8_t>(), stage.flags);
                take(failure, body.get<std::uint8_t>(), stage.unknown);
                open = Open::stage;
            } else if (t == FourCC{"QSDT"}) {
                if (out.stages.empty()) {
                    return false;
                }
                take(failure, body.get<std::uint8_t>(), out.stages.back().log.emplace_back().flags);
                open = Open::log;
            } else if (open == Open::log && t == FourCC{"CNAM"}) {
                take(failure,
                     read_lstring(body, ctx, FourCC{"QUST"}, t, StringKind::description),
                     out.stages.back().log.back().text);
            } else if (open == Open::log && t == FourCC{"NAM0"}) {
                take(failure, read_formid(body), out.stages.back().log.back().next_quest);
            } else if (open == Open::log && t == FourCC{"SCHR"}) {
                take(failure, read_verbatim(body), out.stages.back().log.back().script_header);
            } else if (open == Open::log && t == FourCC{"SCTX"}) {
                std::string_view text;
                take(failure, body.chars(body.remaining()), text);
                out.stages.back().log.back().script_text = std::string(text);
            } else if (open == Open::log && t == FourCC{"QNAM"}) {
                append_formid(failure, out.stages.back().log.back().script_refs, body);
            } else if (t == FourCC{"QOBJ"}) {
                take(failure, body.get<std::uint16_t>(), out.objectives.emplace_back().index);
                open = Open::objective;
            } else if ((open == Open::objective || open == Open::target) &&
                       t == FourCC{"FNAM"}) {
                take(failure, body.get<std::uint32_t>(), out.objectives.back().flags);
            } else if ((open == Open::objective || open == Open::target) &&
                       t == FourCC{"NNAM"}) {
                take(failure, read_lstring(body, ctx, FourCC{"QUST"}, t),
                     out.objectives.back().text);
            } else if ((open == Open::objective || open == Open::target) &&
                       t == FourCC{"QSTA"}) {
                auto& target = out.objectives.back().targets.emplace_back();
                take(failure, body.get<std::int32_t>(), target.alias);
                take(failure, body.get<std::uint32_t>(), target.flags);
                open = Open::target;
            } else if (auto* into = conditions();
                       into != nullptr &&
                       read_condition_field(into->raw, into->strings, field, body, failure)) {
                return true;
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
