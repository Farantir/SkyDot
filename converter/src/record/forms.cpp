// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/forms.hpp"

#include <optional>
#include <utility>

namespace bethconv::record {

io::ParseResult<Static> parse_static(io::SpanReader& data, const FormContext& ctx) {
    Static out;
    const auto walked = walk_fields(
        data, FourCC{"STAT"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"OBND"}) {
                take(failure, read_object_bounds(body), out.bounds);
            } else if (field.type == FourCC{"DNAM"}) {
                take(failure, body.get<float>(), out.max_angle);
                take(failure, read_formid(body), out.material);
                if (!failure && body.remaining() != 0) {
                    take(failure, read_verbatim(body), out.dnam_extra);
                }
            } else if (field.type == FourCC{"MNAM"}) {
                // Four fixed 260-byte NUL-padded slots; empty means unused.
                for (std::size_t i = 0; i < Static::k_lod_count; ++i) {
                    auto slot = body.fixed_string(Static::k_lod_path_size);
                    if (!slot) {
                        if (!failure) {
                            failure = std::move(slot).error();
                        }
                        return true;
                    }
                    out.lod_models[i] = std::string{*slot};
                }
            } else if (!read_model_field(out.model, field, body, failure)) {
                return false;
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Door> parse_door(io::SpanReader& data, const FormContext& ctx) {
    Door out;
    const auto walked = walk_fields(
        data, FourCC{"DOOR"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"OBND"}) {
                take(failure, read_object_bounds(body), out.bounds);
            } else if (field.type == FourCC{"FULL"}) {
                take(failure, read_lstring(body, ctx, FourCC{"DOOR"}, field.type), out.name);
            } else if (field.type == FourCC{"FNAM"}) {
                take(failure, body.get<std::uint8_t>(), out.flags);
            } else if (field.type == FourCC{"SNAM"}) {
                take(failure, read_formid(body), out.open_sound);
            } else if (field.type == FourCC{"ANAM"}) {
                take(failure, read_formid(body), out.close_sound);
            } else if (field.type == FourCC{"BNAM"}) {
                take(failure, read_formid(body), out.loop_sound);
            } else if (field.type == FourCC{"VMAD"}) {
                take(failure, read_script_data(body), out.scripts);
            } else if (!read_model_field(out.model, field, body, failure)) {
                return false;
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Light> parse_light(io::SpanReader& data, const FormContext& ctx) {
    Light out;
    const auto walked = walk_fields(
        data, FourCC{"LIGH"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"OBND"}) {
                take(failure, read_object_bounds(body), out.bounds);
            } else if (field.type == FourCC{"FULL"}) {
                take(failure, read_lstring(body, ctx, FourCC{"LIGH"}, field.type), out.name);
            } else if (field.type == FourCC{"FNAM"}) {
                take(failure, body.get<float>(), out.fade);
            } else if (field.type == FourCC{"DATA"}) {
                take(failure, body.get<std::int32_t>(), out.time);
                take(failure, body.get<std::uint32_t>(), out.radius);
                take(failure, body.get<std::uint32_t>(), out.colour);
                take(failure, body.get<std::uint32_t>(), out.light_flags);
                take(failure, body.get<float>(), out.falloff_exponent);
                take(failure, body.get<float>(), out.fov);
                take(failure, body.get<float>(), out.near_clip);
                take(failure, body.get<float>(), out.flicker_period);
                take(failure, body.get<float>(), out.flicker_intensity_amplitude);
                take(failure, body.get<float>(), out.flicker_movement_amplitude);
                take(failure, body.get<std::uint32_t>(), out.value);
                take(failure, body.get<float>(), out.weight);
            } else if (field.type == FourCC{"VMAD"}) {
                take(failure, read_script_data(body), out.scripts);
            } else if (!read_model_field(out.model, field, body, failure)) {
                return false;
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Cell> parse_cell(io::SpanReader& data, const FormContext& ctx) {
    Cell out;
    const auto walked = walk_fields(
        data, FourCC{"CELL"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"FULL"}) {
                take(failure, read_lstring(body, ctx, FourCC{"CELL"}, field.type), out.name);
            } else if (field.type == FourCC{"DATA"}) {
                // One or two bytes; see Cell::flags.
                auto low = body.get<std::uint8_t>();
                if (!low) {
                    if (!failure) {
                        failure = std::move(low).error();
                    }
                    return true;
                }
                out.flags = *low;
                if (body.remaining() >= 1) {
                    auto high = body.get<std::uint8_t>();
                    if (high) {
                        out.flags = static_cast<std::uint16_t>(
                            out.flags | (static_cast<std::uint16_t>(*high) << 8));
                    }
                }
            } else if (field.type == FourCC{"XCLC"}) {
                Cell::Grid grid;
                take(failure, body.get<std::int32_t>(), grid.x);
                take(failure, body.get<std::int32_t>(), grid.y);
                // The flags word came with the 1.70 header; 8-byte XCLC is a
                // pre-SE file.
                if (!failure && body.remaining() >= sizeof(std::uint32_t)) {
                    take(failure, body.get<std::uint32_t>(), grid.flags);
                }
                if (!failure) {
                    out.grid = grid;
                }
            } else if (field.type == FourCC{"XCLW"}) {
                take(failure, body.get<float>(), out.water_height);
            } else if (field.type == FourCC{"LTMP"}) {
                take(failure, read_formid(body), out.lighting_template);
            } else if (field.type == FourCC{"XLCN"}) {
                take(failure, read_formid(body), out.location);
            } else if (field.type == FourCC{"XCWT"}) {
                take(failure, read_formid(body), out.water);
            } else if (field.type == FourCC{"XCIM"}) {
                take(failure, read_formid(body), out.image_space);
            } else if (field.type == FourCC{"XCMO"}) {
                take(failure, read_formid(body), out.music);
            } else if (field.type == FourCC{"XCAS"}) {
                take(failure, read_formid(body), out.acoustic_space);
            } else if (field.type == FourCC{"XEZN"}) {
                take(failure, read_formid(body), out.encounter_zone);
            } else if (field.type == FourCC{"XOWN"}) {
                take(failure, read_formid(body), out.owner);
            } else if (field.type == FourCC{"LNAM"}) {
                take(failure, read_formid(body), out.sky_lighting);
            } else if (field.type == FourCC{"XCLR"}) {
                take(failure, read_formid_array(body), out.regions);
            } else if (field.type == FourCC{"XCLL"}) {
                take(failure, read_verbatim(body), out.lighting);
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

io::ParseResult<Worldspace> parse_worldspace(io::SpanReader& data, const FormContext& ctx) {
    Worldspace out;
    const auto walked = walk_fields(
        data, FourCC{"WRLD"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"FULL"}) {
                take(failure, read_lstring(body, ctx, FourCC{"WRLD"}, field.type), out.name);
            } else if (field.type == FourCC{"DATA"}) {
                take(failure, body.get<std::uint8_t>(), out.flags);
            } else if (field.type == FourCC{"WNAM"}) {
                take(failure, read_formid(body), out.parent);
            } else if (field.type == FourCC{"PNAM"}) {
                take(failure, body.get<std::uint16_t>(), out.parent_flags);
            } else if (field.type == FourCC{"CNAM"}) {
                take(failure, read_formid(body), out.climate);
            } else if (field.type == FourCC{"NAM2"}) {
                take(failure, read_formid(body), out.water);
            } else if (field.type == FourCC{"NAM3"}) {
                take(failure, read_formid(body), out.lod_water_type);
            } else if (field.type == FourCC{"NAM4"}) {
                take(failure, body.get<float>(), out.lod_water_height);
            } else if (field.type == FourCC{"XLCN"}) {
                take(failure, read_formid(body), out.location);
            } else if (field.type == FourCC{"LTMP"}) {
                take(failure, read_formid(body), out.lighting_template);
            } else if (field.type == FourCC{"XEZN"}) {
                take(failure, read_formid(body), out.encounter_zone);
            } else if (field.type == FourCC{"ZNAM"}) {
                take(failure, read_formid(body), out.music);
            } else if (field.type == FourCC{"DNAM"}) {
                float land{};
                float water{};
                take(failure, body.get<float>(), land);
                take(failure, body.get<float>(), water);
                if (!failure) {
                    out.default_land_height = land;
                    out.default_water_height = water;
                }
            } else if (field.type == FourCC{"WCTR"}) {
                std::int16_t x{};
                std::int16_t y{};
                take(failure, body.get<std::int16_t>(), x);
                take(failure, body.get<std::int16_t>(), y);
                if (!failure) {
                    out.centre_cell = std::pair{x, y};
                }
            } else if (field.type == FourCC{"RNAM"}) {
                Worldspace::LargeRefCell cell;
                take(failure, body.get<std::int16_t>(), cell.grid_y);
                take(failure, body.get<std::int16_t>(), cell.grid_x);
                std::uint32_t declared{};
                take(failure, body.get<std::uint32_t>(), declared);
                if (!failure) {
                    // The count must match the payload exactly, not be
                    // trusted for an allocation.
                    auto count =
                        entry_count(body, Worldspace::k_large_ref_entry_size, "WRLD RNAM");
                    if (!count) {
                        failure = std::move(count).error();
                    } else if (*count != declared) {
                        failure = io::ParseError{.origin = std::string(body.origin()),
                                                 .offset = body.absolute_position(),
                                                 .kind = io::ErrorKind::corrupt,
                                                 .detail = "WRLD RNAM: count disagrees with size"};
                    }
                }
                for (std::size_t i = 0; !failure && i < declared; ++i) {
                    Worldspace::LargeRefCell::Entry entry;
                    take(failure, read_formid(body), entry.ref);
                    take(failure, body.get<std::int16_t>(), entry.grid_y);
                    take(failure, body.get<std::int16_t>(), entry.grid_x);
                    if (!failure) {
                        cell.refs.push_back(entry);
                    }
                }
                if (!failure) {
                    out.large_refs.push_back(std::move(cell));
                }
            } else if (field.type == FourCC{"NAM0"}) {
                take(failure, body.get<float>(), out.min_x);
                take(failure, body.get<float>(), out.min_y);
            } else if (field.type == FourCC{"NAM9"}) {
                take(failure, body.get<float>(), out.max_x);
                take(failure, body.get<float>(), out.max_y);
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

io::ParseResult<Reference> parse_reference(const RecordHeader& header, io::SpanReader& data,
                                           const FormContext& ctx) {
    Reference out;
    out.initially_disabled = has_flag(header.flags, RecordFlag::initially_disabled);
    out.persistent = has_flag(header.flags, RecordFlag::persistent);
    out.deleted = header.is_deleted();

    const auto walked = walk_fields(
        data, FourCC{"REFR"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"NAME"}) {
                take(failure, read_formid(body), out.base);
            } else if (field.type == FourCC{"DATA"}) {
                take(failure, read_vec3(body), out.position);
                take(failure, read_vec3(body), out.rotation);
            } else if (field.type == FourCC{"XSCL"}) {
                take(failure, body.get<float>(), out.scale);
            } else if (field.type == FourCC{"XESP"}) {
                Reference::EnableParent parent;
                take(failure, read_formid(body), parent.parent);
                take(failure, body.get<std::uint32_t>(), parent.flags);
                if (!failure) {
                    out.enable_parent = parent;
                }
            } else if (field.type == FourCC{"XTEL"}) {
                Reference::Teleport teleport;
                take(failure, read_formid(body), teleport.destination_door);
                take(failure, read_vec3(body), teleport.position);
                take(failure, read_vec3(body), teleport.rotation);
                take(failure, body.get<std::uint32_t>(), teleport.flags);
                if (!failure) {
                    out.teleport = teleport;
                }
            } else if (field.type == FourCC{"XPRM"}) {
                Reference::Primitive primitive;
                take(failure, read_vec3(body), primitive.bounds);
                take(failure, body.get<float>(), primitive.red);
                take(failure, body.get<float>(), primitive.green);
                take(failure, body.get<float>(), primitive.blue);
                take(failure, body.get<float>(), primitive.unknown);
                take(failure, body.get<std::uint32_t>(), primitive.type);
                if (!failure) {
                    out.primitive = primitive;
                }
            } else if (field.type == FourCC{"XOWN"}) {
                take(failure, read_formid(body), out.owner);
            } else if (field.type == FourCC{"XEMI"}) {
                take(failure, read_formid(body), out.emittance);
            } else if (field.type == FourCC{"XLRT"}) {
                take(failure, read_formid(body), out.light_ref);
            } else if (field.type == FourCC{"XRDS"}) {
                take(failure, body.get<float>(), out.radius);
                out.has_radius = true;
            } else if (field.type == FourCC{"XLCM"}) {
                take(failure, body.get<std::int32_t>(), out.count);
            } else if (field.type == FourCC{"XLKR"}) {
                // Usually 8 bytes, 4 in ten vanilla records; see
                // Reference::LinkedReference.
                Reference::LinkedReference link;
                if (body.remaining() >= 2 * sizeof(std::uint32_t)) {
                    take(failure, read_formid(body), link.keyword);
                }
                take(failure, read_formid(body), link.target);
                if (!failure) {
                    out.linked_references.push_back(link);
                }
            } else if (field.type == FourCC{"XLIG"}) {
                take(failure, read_verbatim(body), out.light_data);
            } else if (field.type == FourCC{"VMAD"}) {
                take(failure, read_script_data(body), out.scripts);
            } else if (field.type == FourCC{"XAPD"}) {
                take(failure, body.get<std::uint8_t>(), out.activate_parent_flags);
            } else if (field.type == FourCC{"XAPR"}) {
                Reference::ActivateParent parent;
                take(failure, read_formid(body), parent.ref);
                take(failure, body.get<float>(), parent.delay);
                if (!failure) {
                    out.activate_parents.push_back(parent);
                }
            } else if (field.type == FourCC{"XLOC"}) {
                Reference::Lock lock;
                take(failure, body.get<std::uint8_t>(), lock.level);
                skip_bytes(failure, body, 3);
                take(failure, read_formid(body), lock.key);
                take(failure, body.get<std::uint8_t>(), lock.flags);
                skip_bytes(failure, body, 3);
                // The last 8 bytes are undocumented; absent in older files.
                if (body.remaining() == 8) {
                    take(failure, body.get<std::uint32_t>(), lock.unknown[0]);
                    take(failure, body.get<std::uint32_t>(), lock.unknown[1]);
                }
                if (!failure) {
                    out.lock = lock;
                }
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

std::span<const FourCC> defined_types() noexcept {
    // All defined types across the forms_*.cpp files. The census dispatches on
    // this list, so a type missing here would never be checked.
    static constexpr FourCC k_types[] = {
        // forms.cpp
        FourCC{"STAT"}, FourCC{"DOOR"}, FourCC{"LIGH"},
        FourCC{"CELL"}, FourCC{"WRLD"}, FourCC{"REFR"},
        // forms_object.cpp
        FourCC{"TXST"}, FourCC{"ACTI"}, FourCC{"CONT"}, FourCC{"MISC"},
        FourCC{"MSTT"}, FourCC{"FURN"}, FourCC{"FLOR"}, FourCC{"TREE"},
        FourCC{"KEYM"}, FourCC{"ALCH"}, FourCC{"AMMO"}, FourCC{"WEAP"},
        FourCC{"PROJ"}, FourCC{"IDLM"}, FourCC{"LVLN"},
        // forms_world.cpp
        FourCC{"LTEX"}, FourCC{"IMGS"}, FourCC{"VOLI"}, FourCC{"CLMT"}, FourCC{"WTHR"},
        FourCC{"REGN"}, FourCC{"LCTN"}, FourCC{"LAND"}, FourCC{"NAVM"},
        FourCC{"NAVI"}, FourCC{"ACHR"}, FourCC{"SPGD"},
        // forms_game.cpp
        FourCC{"GMST"}, FourCC{"GLOB"}, FourCC{"CLAS"}, FourCC{"FACT"},
        FourCC{"ENCH"}, FourCC{"SPEL"}, FourCC{"NPC_"}, FourCC{"QUST"},
        FourCC{"FLST"}, FourCC{"PACK"},
        // forms_actor.cpp
        FourCC{"ARMO"}, FourCC{"ARMA"}, FourCC{"OTFT"}, FourCC{"LVLI"}, FourCC{"RACE"},
    };
    return k_types;
}

bool is_defined_type(FourCC type) noexcept {
    for (const auto known : defined_types()) {
        if (known == type) {
            return true;
        }
    }
    return false;
}

} // namespace bethconv::record
