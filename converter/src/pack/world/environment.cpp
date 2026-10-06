// SPDX-License-Identifier: GPL-3.0-or-later
#include "environment.hpp"

#include "fb_write.hpp"

#include "bethconv/archive/vpath.hpp"
#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/field_walk.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/forms_world.hpp"
#include "skydot_formats/flags.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace bethconv::pack::detail {
namespace {

using io::FourCC;
using skydot::formats::has_flag;

/// Cloud layers: textures (00TX..), speeds (QNAM, RNAM: one byte each,
/// 127 still), colours by time (PNAM, RGBA), alphas by time (JNAM) and
/// NAM1, whose set bits disable a layer. Layer i is drawn on the i-th
/// shape of meshes/sky/clouds.nif.
void read_clouds(const record::Weather& weather, wfb::WeatherT& out) {
    for (std::size_t i = 0; i < record::Weather::k_cloud_layers; ++i) {
        auto& layer = out.clouds.emplace_back(std::make_unique<wfb::CloudLayerT>());
        layer->texture = texture_vpath(weather.cloud_textures[i]);
        layer->enabled = !layer->texture.empty() &&
                         (weather.cloud_layers_disabled & (1U << i)) == 0;
        const auto speed = [&](const std::vector<std::byte>& raw) {
            return i < raw.size()
                       ? (static_cast<float>(static_cast<std::uint8_t>(raw[i])) - 127.0F) / 127.0F
                       : 0.0F;
        };
        layer->speed_x = speed(weather.cloud_speed_x);
        layer->speed_y = speed(weather.cloud_speed_y);
        io::SpanReader colours(weather.cloud_colours, "WTHR PNAM");
        io::SpanReader alphas(weather.cloud_alphas, "WTHR JNAM");
        (void)colours.skip(i * 16);
        (void)alphas.skip(i * 16);
        for (std::size_t t = 0; t < 4; ++t) {
            layer->colors.push_back(colours.get<std::uint32_t>().value_or(0));
            layer->alphas.push_back(alphas.get<float>().value_or(1.0F));
        }
    }
}

} // namespace

void EnvironmentCollector::collect(const record::MergedRecord& merged, io::SpanReader& data,
                                   const record::FormContext& form_ctx) {
    switch (merged.type.value) {
    case FourCC{"WRLD"}.value:
        on_worldspace(merged, data, form_ctx);
        break;
    case FourCC{"WATR"}.value:
        on_water(merged, data);
        break;
    case FourCC{"CLMT"}.value:
        on_climate(merged, data, form_ctx);
        break;
    case FourCC{"WTHR"}.value:
        on_weather(merged, data, form_ctx);
        break;
    case FourCC{"SPGD"}.value:
        on_precipitation(merged, data, form_ctx);
        break;
    case FourCC{"REGN"}.value:
        on_region(merged, data, form_ctx);
        break;
    case FourCC{"IMGS"}.value:
        on_image_space(merged, data, form_ctx);
        break;
    default:
        break;
    }
}

void EnvironmentCollector::on_worldspace(const record::MergedRecord& merged, io::SpanReader& data,
                                         const record::FormContext& form_ctx) {
    auto w = record::parse_worldspace(data, form_ctx);
    if (!w) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    wfb::WorldspaceT out;
    out.id = merged.form.value;
    out.editor_id = w->editor_id;
    out.parent = shared_.global(merged, w->parent, failed);
    out.parent_flags = static_cast<wfb::ParentFlags>(w->parent_flags);
    out.flags = w->flags;
    if (w->default_land_height && w->default_water_height) {
        out.has_defaults = true;
        out.default_land_height = *w->default_land_height;
        out.default_water_height = *w->default_water_height;
    }
    out.water = shared_.global(merged, w->water, failed);
    out.climate = shared_.global(merged, w->climate, failed);
    out.min_x = w->min_x;
    out.min_y = w->min_y;
    out.max_x = w->max_x;
    out.max_y = w->max_y;
    take_large_lists(merged, *w, failed);
    if (failed) {
        ++shared_.stats().unresolved;
    }
    worlds_[out.id] = std::move(out);
}

/// A WRLD's RNAM lists replace those of the same cell from earlier versions: a
/// plugin that changes a worldspace's large references lists only the cells it
/// changed (Update.esm's Tamriel has 191 of Skyrim.esm's 8,455, Dawnguard's
/// 298), and an empty list empties the cell.
void EnvironmentCollector::take_large_lists(const record::MergedRecord& merged,
                                            const record::Worldspace& w, bool& failed) {
    auto& cells = large_entries_[merged.form.value];
    for (const auto& list : w.large_refs) {
        auto& entries = cells[{list.grid_y, list.grid_x}];
        entries.clear();
        for (const auto& entry : list.refs) {
            entries.push_back({shared_.global(merged, entry.ref, failed), list.grid_y,
                               list.grid_x, entry.grid_y, entry.grid_x});
        }
    }
}

void EnvironmentCollector::collect_large_refs(const record::MergedRecord& merged,
                                              io::SpanReader& data,
                                              const record::FormContext& form_ctx) {
    auto w = record::parse_worldspace(data, form_ctx);
    if (!w) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    take_large_lists(merged, *w, failed);
    if (failed) {
        ++shared_.stats().unresolved;
    }
}

/// WATR is not one of the record layer's types; only what rendering needs
/// is read here. DNAM offsets: UESP's field order, checked against
/// Skyrim.esm (228 bytes, 232 in some SE records).
void EnvironmentCollector::on_water(const record::MergedRecord& merged, io::SpanReader& data) {
    wfb::WaterT out;
    out.id = merged.form.value;
    out.layers.resize(3);
    const auto walked = record::for_each_field(
        data, [&](const record::FieldHeader& field, io::SpanReader& body) {
            if (field.type == FourCC{"EDID"}) {
                out.editor_id = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"ANAM"}) {
                out.opacity = body.get<std::uint8_t>().value_or(0);
            } else if (field.type == FourCC{"FNAM"}) {
                out.flags = body.get<std::uint8_t>().value_or(0);
            } else if (field.type == FourCC{"NNAM"} || field.type == FourCC{"NAM2"} ||
                       field.type == FourCC{"NAM3"} || field.type == FourCC{"NAM4"}) {
                std::string path = archive::normalize_vpath(body.zstring().value_or(""));
                if (path.starts_with("data/")) {
                    path.erase(0, 5);
                }
                out.noise.push_back(texture_vpath(path));
            } else if (field.type == FourCC{"DNAM"} && body.remaining() >= 228) {
                const auto f32 = [&](std::size_t offset) {
                    io::SpanReader r = body;
                    (void)r.skip(offset);
                    return r.get<float>().value_or(0.0F);
                };
                const auto u32 = [&](std::size_t offset) {
                    io::SpanReader r = body;
                    (void)r.skip(offset);
                    return r.get<std::uint32_t>().value_or(0) & 0x00FF'FFFFu;
                };
                out.sun_specular_power = f32(16);
                out.reflectivity = f32(20);
                out.fresnel = f32(24);
                out.fog_near = f32(32);
                out.fog_far = f32(36);
                out.shallow_color = u32(40);
                out.deep_color = u32(44);
                out.reflection_color = u32(48);
                for (std::size_t i = 0; i < 3; ++i) {
                    out.layers[i] = wfb::WaterLayer(f32(100 + 4 * i), f32(112 + 4 * i),
                                                    f32(172 + 4 * i), f32(184 + 4 * i));
                }
                out.refraction_magnitude = f32(152);
                out.specular_power = f32(156);
                out.reflection_magnitude = f32(196);
            }
        });
    if (!walked) {
        ++shared_.stats().parse_errors;
        return;
    }
    waters_[out.id] = std::move(out);
}

void EnvironmentCollector::on_climate(const record::MergedRecord& merged, io::SpanReader& data,
                                      const record::FormContext& form_ctx) {
    auto climate = record::parse_climate(data, form_ctx);
    if (!climate) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    wfb::ClimateT out;
    out.id = merged.form.value;
    out.editor_id = climate->editor_id;
    for (const auto& entry : climate->weathers) {
        out.weathers.emplace_back(shared_.global(merged, entry.weather, failed), entry.chance);
    }
    const auto hours = [](std::uint8_t steps) { return static_cast<float>(steps) / 6.0F; };
    out.sunrise_begin = hours(climate->sunrise_begin);
    out.sunrise_end = hours(climate->sunrise_end);
    out.sunset_begin = hours(climate->sunset_begin);
    out.sunset_end = hours(climate->sunset_end);
    out.sun_texture = texture_vpath(climate->sun_texture);
    out.sun_glare_texture = texture_vpath(climate->sun_glare_texture);
    out.sky = model_vpath(climate->model.path);
    out.volatility = climate->volatility;
    // TNAM's last byte: phase length in days, bit 6 Masser, bit 7 Secunda
    // (UESP, CLMT record).
    const auto moons = climate->moons_and_phase_length;
    out.moons = static_cast<std::uint8_t>(((moons >> 6) & 1U) | (((moons >> 7) & 1U) << 1));
    out.phase_length = static_cast<std::uint8_t>(moons & 0x3FU);
    if (failed) {
        ++shared_.stats().unresolved;
    }
    climates_[out.id] = std::move(out);
}

/// NAM0 is 17 x 4 RGBA colours (272 bytes); FNAM eight floats; DALC 32
/// bytes per time of day, the 24-byte form padded with black.
void EnvironmentCollector::on_weather(const record::MergedRecord& merged, io::SpanReader& data,
                                      const record::FormContext& form_ctx) {
    auto weather = record::parse_weather(data, form_ctx);
    if (!weather) {
        ++shared_.stats().parse_errors;
        return;
    }
    wfb::WeatherT out;
    out.id = merged.form.value;
    out.editor_id = weather->editor_id;
    const auto words = [](std::span<const std::byte> raw, std::size_t count, auto& into) {
        io::SpanReader r(raw, "WTHR");
        for (std::size_t i = 0; i < count; ++i) {
            using T = typename std::remove_reference_t<decltype(into)>::value_type;
            into.push_back(r.get<T>().value_or(T{}));
        }
    };
    if (weather->weather_colours.size() >= 272) {
        words(weather->weather_colours, 68, out.colors);
    }
    if (weather->fog_distance.size() >= 32) {
        words(weather->fog_distance, 8, out.fog);
    }
    for (const auto& block : weather->directional_ambient) {
        if (out.directional_ambient.size() >= 28) {
            break;
        }
        std::vector<std::uint32_t> colours;
        words(block, std::min<std::size_t>(block.size() / 4, 7), colours);
        colours.resize(7, 0);
        out.directional_ambient.insert(out.directional_ambient.end(), colours.begin(),
                                       colours.end());
    }
    read_clouds(*weather, out);
    // DATA, 19 bytes (xEdit's TES5 layout; flags checked against the
    // rain and snow weathers): fractions stored as 0..255.
    if (weather->data.size() >= record::Weather::k_data_size) {
        io::SpanReader r(weather->data, "WTHR DATA");
        std::array<std::uint8_t, record::Weather::k_data_size> d{};
        for (auto& b : d) {
            b = r.get<std::uint8_t>().value_or(0);
        }
        const auto unit = [](std::uint8_t v) { return static_cast<float>(v) / 255.0F; };
        out.wind_speed = unit(d[0]);
        out.transition_delta = unit(d[3]);
        out.sun_glare = unit(d[4]);
        out.sun_damage = unit(d[5]);
        out.precipitation_begin = unit(d[6]);
        out.precipitation_end = unit(d[7]);
        out.thunder_begin = unit(d[8]);
        out.thunder_end = unit(d[9]);
        out.thunder_frequency = unit(d[10]);
        out.classification = static_cast<wfb::WeatherClass>(d[11]);
        out.lightning_color = static_cast<std::uint32_t>(d[12]) |
                              (static_cast<std::uint32_t>(d[13]) << 8) |
                              (static_cast<std::uint32_t>(d[14]) << 16);
        out.wind_direction = static_cast<float>(d[17]) * 360.0F / 256.0F;
        out.wind_direction_range = static_cast<float>(d[18]) * 180.0F / 256.0F;
    }
    bool failed = false;
    out.precipitation = shared_.global(merged, weather->precipitation, failed);
    out.aurora = model_vpath(weather->model.path);
    for (const auto image_space : weather->image_spaces) {
        out.image_spaces.push_back(shared_.global(merged, image_space, failed));
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    weathers_[out.id] = std::move(out);
}

/// IMGS: HNAM, CNAM and TNAM as floats (see world.fbs `ImageSpace`).
void EnvironmentCollector::on_image_space(const record::MergedRecord& merged, io::SpanReader& data,
                                          const record::FormContext& form_ctx) {
    auto image_space = record::parse_image_space(data, form_ctx);
    if (!image_space) {
        ++shared_.stats().parse_errors;
        return;
    }
    wfb::ImageSpaceT out;
    out.id = merged.form.value;
    out.editor_id = image_space->editor_id;
    const auto floats = [](std::span<const std::byte> raw, std::size_t count) {
        std::vector<float> values;
        io::SpanReader r(raw, "IMGS");
        for (std::size_t i = 0; i < count && r.remaining() >= 4; ++i) {
            values.push_back(r.get<float>().value_or(0.0F));
        }
        return values.size() == count ? values : std::vector<float>{};
    };
    out.hdr = floats(image_space->hdr, 9);
    out.cinematic = floats(image_space->cinematic, 3);
    out.tint = floats(image_space->tint, 4);
    image_spaces_[out.id] = std::move(out);
}

void EnvironmentCollector::on_precipitation(const record::MergedRecord& merged,
                                            io::SpanReader& data,
                                            const record::FormContext& form_ctx) {
    auto spgd = record::parse_shader_particle_geometry(data, form_ctx);
    if (!spgd) {
        ++shared_.stats().parse_errors;
        return;
    }
    wfb::PrecipitationT out;
    out.id = merged.form.value;
    out.editor_id = spgd->editor_id;
    out.texture = texture_vpath(spgd->texture);
    out.gravity_velocity = spgd->gravity_velocity;
    out.rotation_velocity = spgd->rotation_velocity;
    out.size_x = spgd->particle_size_x;
    out.size_y = spgd->particle_size_y;
    out.center_offset_min = spgd->center_offset_min;
    out.center_offset_max = spgd->center_offset_max;
    out.rotation_range = spgd->initial_rotation_range;
    out.subtextures_x = spgd->subtextures_x;
    out.subtextures_y = spgd->subtextures_y;
    out.type = static_cast<std::uint8_t>(spgd->type);
    out.box_size = spgd->box_size;
    out.density = spgd->particle_density;
    precipitations_[out.id] = std::move(out);
}

/// Regions with a weather list (RDAT type 3); the others are not needed
/// yet.
void EnvironmentCollector::on_region(const record::MergedRecord& merged, io::SpanReader& data,
                                     const record::FormContext& form_ctx) {
    auto region = record::parse_region(data, form_ctx);
    if (!region) {
        ++shared_.stats().parse_errors;
        return;
    }
    constexpr std::uint32_t k_weather = 3;
    bool failed = false;
    wfb::RegionT out;
    out.id = merged.form.value;
    out.editor_id = region->editor_id;
    out.world = shared_.global(merged, region->worldspace, failed);
    for (const auto& entry : region->entries) {
        if (entry.type != k_weather || entry.weathers.empty()) {
            continue;
        }
        out.weather_priority = entry.priority;
        out.weather_override = (entry.flags & 0x1U) != 0;
        for (const auto& w : entry.weathers) {
            out.weathers.emplace_back(shared_.global(merged, w.weather, failed), w.chance,
                                      shared_.global(merged, w.global, failed));
        }
    }
    if (out.weathers.empty()) {
        return;
    }
    for (const auto& area : region->areas) {
        std::vector<float> points;
        points.reserve(area.points.size() * 2);
        for (const auto& [x, y] : area.points) {
            points.push_back(x);
            points.push_back(y);
        }
        out.areas.push_back(std::make_unique<wfb::RegionAreaT>());
        out.areas.back()->points = std::move(points);
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    regions_[out.id] = std::move(out);
}

/// The worldspace's large references, from the winning WRLD's RNAM lists and
/// the references of the merged cells (the winning REFR of each). Choices:
///  - A reference is kept only if a merged cell of this worldspace holds it:
///    one that is deleted (not in any cell), moved to another worldspace, or
///    absent from the load order is dropped and counted.
///  - One that is initially disabled is dropped: nothing in the pack says it
///    is ever enabled, and the engine does not run enable-state scripts for
///    it. A REFR record in a later plugin that clears the flag wins, so it
///    stays; the XESP enable parent is carried for the engine instead.
///  - The placement is the winner's, not RNAM's: a plugin that moves a large
///    reference moves it here too. The lists per cell stay the ones the
///    Creation Kit made for the original position, as the game's do.
///  - The lists come from every version of the WRLD (take_large_lists), a cell
///    decided by the last plugin that lists it.
void EnvironmentCollector::resolve_large_refs(const wfb::WorldT& world, wfb::WorldspaceT& out,
                                              const std::map<std::pair<std::int16_t, std::int16_t>,
                                                             std::vector<LargeEntry>>& lists) {
    if (lists.empty()) {
        return;
    }
    // The references of this worldspace's cells, by id.
    std::map<std::uint32_t, const wfb::Ref*> placed;
    for (const auto& cell : world.cells) {
        if (cell->world != out.id) {
            continue;
        }
        for (const auto& ref : cell->refs) {
            placed.emplace(ref.id(), &ref);
        }
    }
    auto& stats = shared_.stats();
    std::map<std::uint32_t, std::uint32_t> index; // ref id -> index in out.large_refs
    std::map<std::pair<std::int16_t, std::int16_t>, std::vector<std::uint32_t>> cell_lists;
    std::map<std::uint32_t, bool> dropped;
    for (const auto& [list_cell, entries] : lists) {
    for (const auto& entry : entries) {
        auto found = index.find(entry.ref);
        if (found == index.end()) {
            if (dropped.contains(entry.ref)) {
                continue;
            }
            const auto at = placed.find(entry.ref);
            if (at == placed.end()) {
                dropped.emplace(entry.ref, true);
                ++stats.large_refs_dropped;
                continue;
            }
            const wfb::Ref& ref = *at->second;
            if (has_flag(ref.flags(), wfb::RefFlags::initially_disabled)) {
                dropped.emplace(entry.ref, true);
                ++stats.large_refs_dropped;
                continue;
            }
            found = index.emplace(entry.ref, static_cast<std::uint32_t>(out.large_refs.size()))
                        .first;
            out.large_refs.emplace_back(ref.id(), ref.base(), ref.position(), ref.rotation(),
                                        ref.scale(), ref.flags(), ref.enable_parent(),
                                        entry.cell_x, entry.cell_y);
        }
        cell_lists[list_cell].push_back(found->second);
    }
    }
    // Indices follow ids once the references are sorted by id; sort them and
    // renumber.
    std::vector<std::uint32_t> order(out.large_refs.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::ranges::sort(order, [&](std::uint32_t a, std::uint32_t b) {
        return out.large_refs[a].id() < out.large_refs[b].id();
    });
    std::vector<std::uint32_t> renumber(order.size());
    std::vector<wfb::LargeRef> sorted;
    sorted.reserve(order.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        renumber[order[i]] = i;
        sorted.push_back(out.large_refs[order[i]]);
    }
    out.large_refs = std::move(sorted);
    for (auto& [cell, refs] : cell_lists) { // std::map: ordered by (y, x)
        for (auto& r : refs) {
            r = renumber[r];
        }
        std::ranges::sort(refs);
        refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
        out.large_cells.emplace_back(cell.second, cell.first,
                                     static_cast<std::uint32_t>(out.large_cell_refs.size()),
                                     static_cast<std::uint32_t>(refs.size()));
        out.large_cell_refs.insert(out.large_cell_refs.end(), refs.begin(), refs.end());
    }
    stats.large_refs += out.large_refs.size();
    stats.large_ref_cells += out.large_cells.size();
}

void EnvironmentCollector::finish(wfb::WorldT& world) {
    for (auto& [id, out] : worlds_) {
        resolve_large_refs(world, out, large_entries_[id]);
    }
    auto& stats = shared_.stats();
    stats.worlds += worlds_.size();
    stats.waters += waters_.size();
    stats.climates += climates_.size();
    stats.weathers += weathers_.size();
    stats.image_spaces += image_spaces_.size();
    stats.precipitations += precipitations_.size();
    stats.regions += regions_.size();
    move_into(world.worlds, worlds_);
    move_into(world.waters, waters_);
    move_into(world.climates, climates_);
    move_into(world.weathers, weathers_);
    move_into(world.image_spaces, image_spaces_);
    move_into(world.precipitations, precipitations_);
    move_into(world.regions, regions_);
}

} // namespace bethconv::pack::detail
