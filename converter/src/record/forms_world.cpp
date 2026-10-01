// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/record/forms_world.hpp"

#include <optional>
#include <utility>

namespace bethconv::record {
namespace {

/// Read a fixed-stride array field into a list; a payload that is not a whole
/// number of entries is an error rather than rounded down.
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

/// LCTN's 12-byte reference shape, shared by six field types.
void read_cell_ref(std::optional<io::ParseError>& failure, std::vector<Location::CellRef>& into,
                   io::SpanReader& body, std::string_view what) {
    read_entries(failure, into, body, Location::k_cell_ref_size, what,
                 [&](Location::CellRef& entry) {
                     take(failure, read_formid(body), entry.a);
                     take(failure, read_formid(body), entry.b);
                     take(failure, body.get<std::int16_t>(), entry.grid_x);
                     take(failure, body.get<std::int16_t>(), entry.grid_y);
                 });
}

/// LCTN's 16-byte reference shape, shared by ACSR and LCSR.
void read_static_ref(std::optional<io::ParseError>& failure,
                     std::vector<Location::StaticRef>& into, io::SpanReader& body,
                     std::string_view what) {
    read_entries(failure, into, body, Location::k_static_ref_size, what,
                 [&](Location::StaticRef& entry) {
                     take(failure, read_formid(body), entry.a);
                     take(failure, read_formid(body), entry.b);
                     take(failure, read_formid(body), entry.c);
                     take(failure, body.get<std::int16_t>(), entry.grid_x);
                     take(failure, body.get<std::int16_t>(), entry.grid_y);
                 });
}

/// LCTN's "one FormID, then N cell coordinates" layout (ACEC, LCEC, RCEC). At
/// least 8 bytes, then multiples of 4.
void read_cell_list(std::optional<io::ParseError>& failure,
                    std::vector<Location::CellList>& into, io::SpanReader& body,
                    std::string_view what) {
    if (body.remaining() < Location::k_cell_list_header ||
        (body.remaining() - Location::k_cell_list_header) % Location::k_cell_coord_size != 0) {
        if (!failure) {
            failure = body
                          .fail(io::ErrorKind::bad_value,
                                std::string{what} + " of " + std::to_string(body.remaining()) +
                                    " bytes is not a FormID followed by whole cell coordinates")
                          .error();
        }
        return;
    }
    Location::CellList list;
    take(failure, read_formid(body), list.owner);
    while (!failure && body.remaining() >= Location::k_cell_coord_size) {
        std::int16_t x{};
        std::int16_t y{};
        take(failure, body.get<std::int16_t>(), x);
        take(failure, body.get<std::int16_t>(), y);
        if (!failure) {
            list.cells.emplace_back(x, y);
        }
    }
    if (!failure) {
        into.push_back(std::move(list));
    }
}

/// A FormID array appended to a list that may receive several fields.
void append_formids(std::optional<io::ParseError>& failure, std::vector<FormId>& into,
                    io::SpanReader& body) {
    auto more = read_formid_array(body);
    if (!more) {
        if (!failure) {
            failure = std::move(more).error();
        }
        return;
    }
    into.insert(into.end(), more->begin(), more->end());
}

/// WLST and RDWT share the 12-byte weather entry.
void read_weather_entries(std::optional<io::ParseError>& failure,
                          std::vector<Climate::WeatherEntry>& into, io::SpanReader& body,
                          std::string_view what) {
    read_entries(failure, into, body, Climate::k_weather_entry_size, what,
                 [&](Climate::WeatherEntry& entry) {
                     take(failure, read_formid(body), entry.weather);
                     take(failure, body.get<std::int32_t>(), entry.chance);
                     take(failure, read_formid(body), entry.global);
                 });
}

} // namespace

io::ParseResult<LandTexture> parse_land_texture(io::SpanReader& data, const FormContext& ctx) {
    LandTexture out;
    const auto walked = walk_fields(
        data, FourCC{"LTEX"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"TNAM"}) {
                take(failure, read_formid(body), out.texture_set);
            } else if (field.type == FourCC{"MNAM"}) {
                take(failure, read_formid(body), out.material_type);
            } else if (field.type == FourCC{"HNAM"}) {
                take(failure, body.get<std::uint8_t>(), out.friction);
                take(failure, body.get<std::uint8_t>(), out.restitution);
            } else if (field.type == FourCC{"SNAM"}) {
                take(failure, body.get<std::uint8_t>(), out.specular);
            } else if (field.type == FourCC{"GNAM"}) {
                append_formids(failure, out.grasses, body);
            } else if (field.type == FourCC{"INAM"}) {
                take(failure, body.get<std::uint32_t>(), out.inam);
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

io::ParseResult<ImageSpace> parse_image_space(io::SpanReader& data, const FormContext& ctx) {
    ImageSpace out;
    const auto walked = walk_fields(
        data, FourCC{"IMGS"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"HNAM"}) {
                take(failure, read_verbatim(body), out.hdr);
            } else if (field.type == FourCC{"CNAM"}) {
                take(failure, read_verbatim(body), out.cinematic);
            } else if (field.type == FourCC{"TNAM"}) {
                take(failure, read_verbatim(body), out.tint);
            } else if (field.type == FourCC{"DNAM"}) {
                take(failure, read_verbatim(body), out.depth_of_field);
            } else if (field.type == FourCC{"ENAM"}) {
                take(failure, read_verbatim(body), out.legacy);
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

io::ParseResult<Climate> parse_climate(io::SpanReader& data, const FormContext& ctx) {
    Climate out;
    const auto walked = walk_fields(
        data, FourCC{"CLMT"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"WLST"}) {
                read_weather_entries(failure, out.weathers, body, "CLMT WLST");
            } else if (field.type == FourCC{"FNAM"}) {
                take(failure, read_zstring(body), out.sun_texture);
            } else if (field.type == FourCC{"GNAM"}) {
                take(failure, read_zstring(body), out.sun_glare_texture);
            } else if (field.type == FourCC{"TNAM"}) {
                take(failure, body.get<std::uint8_t>(), out.sunrise_begin);
                take(failure, body.get<std::uint8_t>(), out.sunrise_end);
                take(failure, body.get<std::uint8_t>(), out.sunset_begin);
                take(failure, body.get<std::uint8_t>(), out.sunset_end);
                take(failure, body.get<std::uint8_t>(), out.volatility);
                take(failure, body.get<std::uint8_t>(), out.moons_and_phase_length);
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

io::ParseResult<Weather> parse_weather(io::SpanReader& data, const FormContext& ctx) {
    Weather out;
    const auto walked = walk_fields(
        data, FourCC{"WTHR"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            // Cloud-layer tags are computed: 0x30 + layer, then "0TX".
            const auto tag = field.type.to_string();
            if (tag.size() == 4 && tag[1] == '0' && tag[2] == 'T' && tag[3] == 'X') {
                const auto index =
                    static_cast<std::size_t>(static_cast<unsigned char>(tag[0]) -
                                             Weather::k_cloud_tag_base);
                if (index < Weather::k_cloud_layers) {
                    take(failure, read_zstring(body), out.cloud_textures[index]);
                    return true;
                }
                return false;
            }
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"DATA"}) {
                take(failure, read_verbatim(body), out.data);
            } else if (field.type == FourCC{"NAM0"}) {
                take(failure, read_verbatim(body), out.weather_colours);
            } else if (field.type == FourCC{"PNAM"}) {
                take(failure, read_verbatim(body), out.cloud_colours);
            } else if (field.type == FourCC{"JNAM"}) {
                take(failure, read_verbatim(body), out.cloud_alphas);
            } else if (field.type == FourCC{"QNAM"}) {
                take(failure, read_verbatim(body), out.cloud_speed_x);
            } else if (field.type == FourCC{"RNAM"}) {
                take(failure, read_verbatim(body), out.cloud_speed_y);
            } else if (field.type == FourCC{"FNAM"}) {
                take(failure, read_verbatim(body), out.fog_distance);
            } else if (field.type == FourCC{"DALC"}) {
                std::vector<std::byte> block;
                take(failure, read_verbatim(body), block);
                if (!failure) {
                    out.directional_ambient.push_back(std::move(block));
                }
            } else if (field.type == FourCC{"NAM1"}) {
                take(failure, body.get<std::uint32_t>(), out.cloud_layers_disabled);
            } else if (field.type == FourCC{"LNAM"}) {
                take(failure, body.get<std::uint32_t>(), out.lnam);
            } else if (field.type == FourCC{"ONAM"}) {
                take(failure, body.get<std::uint32_t>(), out.onam);
            } else if (field.type == FourCC{"MNAM"}) {
                take(failure, read_formid(body), out.precipitation);
            } else if (field.type == FourCC{"NNAM"}) {
                take(failure, read_formid(body), out.visual_effect);
            } else if (field.type == FourCC{"TNAM"}) {
                append_formids(failure, out.sky_statics, body);
            } else if (field.type == FourCC{"HNAM"}) {
                append_formids(failure, out.volumetric_lighting, body);
            } else if (field.type == FourCC{"IMSP"}) {
                append_formids(failure, out.image_spaces, body);
            } else if (field.type == FourCC{"SNAM"}) {
                read_entries(failure, out.sounds, body, Weather::k_sound_entry_size, "WTHR SNAM",
                             [&](Weather::SoundEntry& entry) {
                                 take(failure, read_formid(body), entry.sound);
                                 take(failure, body.get<std::uint32_t>(), entry.type);
                             });
            } else if (field.type == FourCC{"NAM2"}) {
                take(failure, read_verbatim(body), out.nam2);
            } else if (field.type == FourCC{"NAM3"}) {
                take(failure, read_verbatim(body), out.nam3);
            } else if (field.type == FourCC{"ANAM"}) {
                take(failure, read_zstring(body), out.anam);
            } else if (field.type == FourCC{"BNAM"}) {
                take(failure, read_zstring(body), out.bnam);
            } else if (field.type == FourCC{"CNAM"}) {
                take(failure, read_zstring(body), out.cnam);
            } else if (field.type == FourCC{"DNAM"}) {
                take(failure, read_zstring(body), out.dnam);
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

io::ParseResult<Region> parse_region(io::SpanReader& data, const FormContext& ctx) {
    Region out;
    const auto walked = walk_fields(
        data, FourCC{"REGN"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"EDID"}) {
                take(failure, read_zstring(body), out.editor_id);
            } else if (field.type == FourCC{"RCLR"}) {
                take(failure, body.get<std::uint32_t>(), out.map_colour);
            } else if (field.type == FourCC{"WNAM"}) {
                take(failure, read_formid(body), out.worldspace);
            } else if (field.type == FourCC{"ICON"}) {
                take(failure, read_zstring(body), out.icon);
            } else if (field.type == FourCC{"RPLI"}) {
                // RPLI opens an area; the following RPLD fills it.
                Region::Area area;
                take(failure, body.get<std::uint32_t>(), area.edge_fall_off);
                if (!failure) {
                    out.areas.push_back(std::move(area));
                }
            } else if (field.type == FourCC{"RPLD"}) {
                if (out.areas.empty()) {
                    out.areas.emplace_back();
                }
                read_entries(failure, out.areas.back().points, body, Region::k_point_size,
                             "REGN RPLD", [&](std::pair<float, float>& point) {
                                 take(failure, body.get<float>(), point.first);
                                 take(failure, body.get<float>(), point.second);
                             });
            } else if (field.type == FourCC{"RDAT"}) {
                Region::Data entry;
                take(failure, body.get<std::uint32_t>(), entry.type);
                take(failure, body.get<std::uint8_t>(), entry.flags);
                take(failure, body.get<std::uint8_t>(), entry.priority);
                take(failure, body.get<std::uint16_t>(), entry.unknown);
                if (!failure) {
                    out.entries.push_back(std::move(entry));
                }
            } else if (field.type == FourCC{"RDMO"} || field.type == FourCC{"RDMP"} ||
                       field.type == FourCC{"RDSA"} || field.type == FourCC{"RDWT"} ||
                       field.type == FourCC{"RDOT"}) {
                // Fields after an RDAT belong to that entry.
                if (out.entries.empty()) {
                    out.entries.emplace_back();
                }
                Region::Data& entry = out.entries.back();
                if (field.type == FourCC{"RDMO"}) {
                    take(failure, read_formid(body), entry.music);
                } else if (field.type == FourCC{"RDMP"}) {
                    take(failure, read_lstring(body, ctx, FourCC{"REGN"}, field.type),
                         entry.map_name);
                } else if (field.type == FourCC{"RDSA"}) {
                    read_entries(failure, entry.sounds, body, Region::k_sound_entry_size,
                                 "REGN RDSA", [&](Region::Data::Sound& sound) {
                                     take(failure, read_formid(body), sound.sound);
                                     take(failure, body.get<std::uint32_t>(), sound.flags);
                                     take(failure, body.get<float>(), sound.chance);
                                 });
                } else if (field.type == FourCC{"RDWT"}) {
                    read_weather_entries(failure, entry.weathers, body, "REGN RDWT");
                } else {
                    entry.has_objects = true;
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

io::ParseResult<Location> parse_location(io::SpanReader& data, const FormContext& ctx) {
    Location out;
    const BaseObjectFields into{
        .editor_id = &out.editor_id, .name = &out.name, .keywords = &out.keywords};
    const auto walked = walk_fields(
        data, FourCC{"LCTN"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"PNAM"}) {
                take(failure, read_formid(body), out.parent);
            } else if (field.type == FourCC{"NAM1"}) {
                take(failure, read_formid(body), out.music);
            } else if (field.type == FourCC{"FNAM"}) {
                take(failure, read_formid(body), out.unreported_crime_faction);
            } else if (field.type == FourCC{"MNAM"}) {
                take(failure, read_formid(body), out.world_location_marker);
            } else if (field.type == FourCC{"RNAM"}) {
                take(failure, body.get<float>(), out.world_location_radius);
            } else if (field.type == FourCC{"NAM0"}) {
                take(failure, read_formid(body), out.horse_marker);
            } else if (field.type == FourCC{"CNAM"}) {
                take(failure, body.get<std::uint32_t>(), out.colour);
            } else if (field.type == FourCC{"ACPR"}) {
                read_cell_ref(failure, out.actor_cell_persistent, body, "LCTN ACPR");
            } else if (field.type == FourCC{"LCPR"}) {
                read_cell_ref(failure, out.location_cell_persistent, body, "LCTN LCPR");
            } else if (field.type == FourCC{"ACEP"}) {
                read_cell_ref(failure, out.actor_cell_encounter, body, "LCTN ACEP");
            } else if (field.type == FourCC{"LCEP"}) {
                read_cell_ref(failure, out.location_cell_encounter, body, "LCTN LCEP");
            } else if (field.type == FourCC{"ACUN"}) {
                read_cell_ref(failure, out.actor_cell_unique, body, "LCTN ACUN");
            } else if (field.type == FourCC{"LCUN"}) {
                read_cell_ref(failure, out.location_cell_unique, body, "LCTN LCUN");
            } else if (field.type == FourCC{"ACSR"}) {
                read_static_ref(failure, out.actor_cell_static, body, "LCTN ACSR");
            } else if (field.type == FourCC{"LCSR"}) {
                read_static_ref(failure, out.location_cell_static, body, "LCTN LCSR");
            } else if (field.type == FourCC{"ACEC"}) {
                read_cell_list(failure, out.actor_cell_marker, body, "LCTN ACEC");
            } else if (field.type == FourCC{"LCEC"}) {
                read_cell_list(failure, out.location_cell_marker, body, "LCTN LCEC");
            } else if (field.type == FourCC{"RCEC"}) {
                read_cell_list(failure, out.ref_cell_marker, body, "LCTN RCEC");
            } else if (field.type == FourCC{"ACID"}) {
                append_formids(failure, out.actor_ids, body);
            } else if (field.type == FourCC{"LCID"}) {
                append_formids(failure, out.location_ids, body);
            } else if (field.type == FourCC{"RCPR"}) {
                append_formids(failure, out.ref_persistent, body);
            } else {
                return read_base_object_field(into, FourCC{"LCTN"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

io::ParseResult<Landscape> parse_landscape(io::SpanReader& data, const FormContext& ctx) {
    Landscape out;
    const auto walked = walk_fields(
        data, FourCC{"LAND"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"DATA"}) {
                take(failure, body.get<std::uint32_t>(), out.flags);
            } else if (field.type == FourCC{"VNML"}) {
                take(failure, read_verbatim(body), out.normals);
            } else if (field.type == FourCC{"VHGT"}) {
                take(failure, read_verbatim(body), out.heights);
            } else if (field.type == FourCC{"VCLR"}) {
                take(failure, read_verbatim(body), out.vertex_colours);
            } else if (field.type == FourCC{"BTXT"} || field.type == FourCC{"ATXT"}) {
                Landscape::TextureLayer layer;
                take(failure, read_formid(body), layer.texture);
                take(failure, body.get<std::uint8_t>(), layer.quadrant);
                take(failure, body.get<std::uint8_t>(), layer.unknown);
                take(failure, body.get<std::int16_t>(), layer.layer);
                if (!failure) {
                    if (field.type == FourCC{"BTXT"}) {
                        out.base_layers.push_back(std::move(layer));
                    } else {
                        out.additional_layers.push_back(std::move(layer));
                    }
                }
            } else if (field.type == FourCC{"VTXT"}) {
                // VTXT belongs to the preceding ATXT; without one it opens its
                // own layer rather than being dropped.
                if (out.additional_layers.empty()) {
                    out.additional_layers.emplace_back();
                }
                auto count = entry_count(body, Landscape::k_alpha_point_size, "LAND VTXT");
                if (!count) {
                    if (!failure) {
                        failure = std::move(count).error();
                    }
                    return true;
                }
                take(failure, read_verbatim(body), out.additional_layers.back().alpha_map);
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

io::ParseResult<NavMesh> parse_nav_mesh(io::SpanReader& data, const FormContext& ctx) {
    NavMesh out;
    const auto walked = walk_fields(
        data, FourCC{"NAVM"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"NVNM"}) {
                take(failure, read_verbatim(body), out.geometry);
            } else if (field.type == FourCC{"ONAM"}) {
                take(failure, read_verbatim(body), out.onam);
            } else if (field.type == FourCC{"PNAM"}) {
                take(failure, read_verbatim(body), out.pnam);
            } else if (field.type == FourCC{"NNAM"}) {
                take(failure, read_verbatim(body), out.nnam);
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

io::ParseResult<NavigationIndex> parse_navigation_index(io::SpanReader& data,
                                                        const FormContext& ctx) {
    NavigationIndex out;
    const auto walked = walk_fields(
        data, FourCC{"NAVI"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"NVER"}) {
                take(failure, body.get<std::uint32_t>(), out.version);
            } else if (field.type == FourCC{"NVMI"} || field.type == FourCC{"NVSI"}) {
                std::vector<std::byte> block;
                take(failure, read_verbatim(body), block);
                if (!failure) {
                    if (field.type == FourCC{"NVMI"}) {
                        out.mesh_info.push_back(std::move(block));
                    } else {
                        out.islands.push_back(std::move(block));
                    }
                }
            } else if (field.type == FourCC{"NVPP"}) {
                take(failure, read_verbatim(body), out.preferred_paths);
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

io::ParseResult<ActorReference> parse_actor_reference(const RecordHeader& header,
                                                      io::SpanReader& data,
                                                      const FormContext& ctx) {
    ActorReference out;
    out.initially_disabled = has_flag(header.flags, RecordFlag::initially_disabled);
    out.persistent = has_flag(header.flags, RecordFlag::persistent);
    out.deleted = header.is_deleted();

    const BaseObjectFields into{.editor_id = &out.editor_id, .scripts = &out.scripts};
    const auto walked = walk_fields(
        data, FourCC{"ACHR"}, ctx,
        [&](const FieldHeader& field, io::SpanReader& body,
            std::optional<io::ParseError>& failure) {
            if (field.type == FourCC{"NAME"}) {
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
            } else if (field.type == FourCC{"XLKR"}) {
                // Same 8-or-4 byte split as REFR: 14,697 vanilla ACHR XLKRs are
                // 8 bytes, four are 4.
                Reference::LinkedReference link;
                if (body.remaining() >= 2 * sizeof(std::uint32_t)) {
                    take(failure, read_formid(body), link.keyword);
                }
                take(failure, read_formid(body), link.target);
                if (!failure) {
                    out.linked_references.push_back(link);
                }
            } else if (field.type == FourCC{"XLRT"}) {
                take(failure, read_formid(body), out.light_ref);
            } else if (field.type == FourCC{"XLCM"}) {
                take(failure, body.get<std::int32_t>(), out.count);
            } else if (field.type == FourCC{"XLCN"}) {
                take(failure, read_formid(body), out.location);
            } else if (field.type == FourCC{"XOWN"}) {
                take(failure, read_formid(body), out.owner);
            } else if (field.type == FourCC{"XEZN"}) {
                take(failure, read_formid(body), out.encounter_zone);
            } else if (field.type == FourCC{"INAM"}) {
                take(failure, read_formid(body), out.ignored_by_sandbox);
            } else if (field.type == FourCC{"XHOR"}) {
                take(failure, read_formid(body), out.horse);
            } else if (field.type == FourCC{"XLRL"}) {
                take(failure, read_formid(body), out.location_ref);
            } else if (field.type == FourCC{"XPRD"}) {
                take(failure, body.get<float>(), out.patrol_idle_time);
            } else if (field.type == FourCC{"XAPD"}) {
                take(failure, body.get<std::uint8_t>(), out.activate_parent_flags);
            } else if (field.type == FourCC{"XAPR"}) {
                ActorReference::ActivateParent parent;
                take(failure, read_formid(body), parent.reference);
                take(failure, body.get<float>(), parent.delay);
                if (!failure) {
                    out.activate_parents.push_back(parent);
                }
            } else if (field.type == FourCC{"XRGD"}) {
                take(failure, read_verbatim(body), out.ragdoll);
            } else if (field.type == FourCC{"XRGB"}) {
                take(failure, read_verbatim(body), out.ragdoll_bones);
            } else if (field.type == FourCC{"PDTO"}) {
                take(failure, read_verbatim(body), out.topic_override);
            } else if (field.type == FourCC{"XPPA"}) {
                out.is_patrol = true;
            } else if (field.type == FourCC{"XIS2"}) {
                out.ignored_by_sandbox_marker = true;
            } else {
                return read_base_object_field(into, FourCC{"ACHR"}, field, body, ctx, failure);
            }
            return true;
        });
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return out;
}

} // namespace bethconv::record
