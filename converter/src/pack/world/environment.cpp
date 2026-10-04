// SPDX-License-Identifier: GPL-3.0-or-later
#include "environment.hpp"

#include "bethconv/archive/vpath.hpp"
#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/field_walk.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/forms_world.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace bethconv::pack::detail {
namespace {

using io::FourCC;

/// Cloud layers: textures (00TX..), speeds (QNAM, RNAM: one byte each,
/// 127 still), colours by time (PNAM, RGBA), alphas by time (JNAM) and
/// NAM1, whose set bits disable a layer. Layer i is drawn on the i-th
/// shape of meshes/sky/clouds.nif.
void read_clouds(const record::Weather& weather, WorldWeather& out) {
    out.clouds.resize(record::Weather::k_cloud_layers);
    for (std::size_t i = 0; i < out.clouds.size(); ++i) {
        auto& layer = out.clouds[i];
        layer.texture = texture_vpath(weather.cloud_textures[i]);
        layer.enabled = !layer.texture.empty() &&
                        (weather.cloud_layers_disabled & (1U << i)) == 0;
        const auto speed = [&](const std::vector<std::byte>& raw) {
            return i < raw.size()
                       ? (static_cast<float>(static_cast<std::uint8_t>(raw[i])) - 127.0F) / 127.0F
                       : 0.0F;
        };
        layer.speed_x = speed(weather.cloud_speed_x);
        layer.speed_y = speed(weather.cloud_speed_y);
        io::SpanReader colours(weather.cloud_colours, "WTHR PNAM");
        io::SpanReader alphas(weather.cloud_alphas, "WTHR JNAM");
        (void)colours.skip(i * 16);
        (void)alphas.skip(i * 16);
        for (std::size_t t = 0; t < 4; ++t) {
            layer.colors[t] = colours.get<std::uint32_t>().value_or(0);
            layer.alphas[t] = alphas.get<float>().value_or(1.0F);
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
    Worldspace out{
        .id = merged.form.value,
        .editor_id = w->editor_id,
        .parent = shared_.global(merged, w->parent, failed),
        .parent_flags = static_cast<wfb::ParentFlags>(w->parent_flags),
        .flags = w->flags,
        .defaults = std::nullopt,
        .water = shared_.global(merged, w->water, failed),
        .climate = shared_.global(merged, w->climate, failed),
        .bounds = {w->min_x, w->min_y, w->max_x, w->max_y},
    };
    if (w->default_land_height && w->default_water_height) {
        out.defaults = std::array{*w->default_land_height, *w->default_water_height};
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    worlds_[out.id] = std::move(out);
}

/// WATR is not one of the record layer's types; only what rendering needs
/// is read here. DNAM offsets: UESP's field order, checked against
/// Skyrim.esm (228 bytes, 232 in some SE records).
void EnvironmentCollector::on_water(const record::MergedRecord& merged, io::SpanReader& data) {
    WorldWater out;
    out.id = merged.form.value;
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
                    out.layers[i] = WorldWater::Layer{
                        .wind_direction = f32(100 + 4 * i),
                        .wind_speed = f32(112 + 4 * i),
                        .uv_scale = f32(172 + 4 * i),
                        .amplitude = f32(184 + 4 * i),
                    };
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
    WorldClimate out;
    out.id = merged.form.value;
    out.editor_id = climate->editor_id;
    for (const auto& entry : climate->weathers) {
        out.weathers.emplace_back(shared_.global(merged, entry.weather, failed), entry.chance);
    }
    const auto hours = [](std::uint8_t steps) { return static_cast<float>(steps) / 6.0F; };
    out.sun = {hours(climate->sunrise_begin), hours(climate->sunrise_end),
               hours(climate->sunset_begin), hours(climate->sunset_end)};
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
    WorldWeather out;
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
    WorldImageSpace out;
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
    precipitations_[merged.form.value] = WorldPrecipitation{
        .id = merged.form.value,
        .editor_id = spgd->editor_id,
        .texture = texture_vpath(spgd->texture),
        .gravity_velocity = spgd->gravity_velocity,
        .rotation_velocity = spgd->rotation_velocity,
        .size_x = spgd->particle_size_x,
        .size_y = spgd->particle_size_y,
        .center_offset_min = spgd->center_offset_min,
        .center_offset_max = spgd->center_offset_max,
        .rotation_range = spgd->initial_rotation_range,
        .subtextures_x = spgd->subtextures_x,
        .subtextures_y = spgd->subtextures_y,
        .type = static_cast<std::uint8_t>(spgd->type),
        .box_size = spgd->box_size,
        .density = spgd->particle_density,
    };
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
    WorldRegion out;
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
            out.weathers.push_back({.weather = shared_.global(merged, w.weather, failed),
                                    .chance = w.chance,
                                    .global = shared_.global(merged, w.global, failed)});
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
        out.areas.push_back(std::move(points));
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    regions_[out.id] = std::move(out);
}

std::vector<flatbuffers::Offset<wfb::Worldspace>> EnvironmentCollector::write_worlds(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::Worldspace>> worlds;
    for (const auto& [id, w] : worlds_) {
        const auto editor_id = builder.CreateString(w.editor_id);
        wfb::WorldspaceBuilder wsb(builder);
        wsb.add_id(w.id);
        wsb.add_editor_id(editor_id);
        wsb.add_parent(w.parent);
        wsb.add_parent_flags(w.parent_flags);
        wsb.add_flags(w.flags);
        if (w.defaults) {
            wsb.add_has_defaults(true);
            wsb.add_default_land_height((*w.defaults)[0]);
            wsb.add_default_water_height((*w.defaults)[1]);
        }
        wsb.add_water(w.water);
        wsb.add_climate(w.climate);
        wsb.add_min_x(w.bounds[0]);
        wsb.add_min_y(w.bounds[1]);
        wsb.add_max_x(w.bounds[2]);
        wsb.add_max_y(w.bounds[3]);
        worlds.push_back(wsb.Finish());
        ++stats.worlds;
    }
    return worlds;
}

std::vector<flatbuffers::Offset<wfb::Water>> EnvironmentCollector::write_waters(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::Water>> waters;
    for (const auto& [id, w] : waters_) {
        std::vector<wfb::WaterLayer> layers;
        for (const auto& l : w.layers) {
            layers.emplace_back(l.wind_direction, l.wind_speed, l.uv_scale, l.amplitude);
        }
        std::vector<flatbuffers::Offset<flatbuffers::String>> noise;
        for (const auto& path : w.noise) {
            noise.push_back(builder.CreateString(path));
        }
        const auto editor_id = builder.CreateString(w.editor_id);
        const auto layers_off = builder.CreateVectorOfStructs(layers);
        const auto noise_off = builder.CreateVector(noise);
        wfb::WaterBuilder wb2(builder);
        wb2.add_id(w.id);
        wb2.add_editor_id(editor_id);
        wb2.add_opacity(w.opacity);
        wb2.add_flags(w.flags);
        wb2.add_shallow_color(w.shallow_color);
        wb2.add_deep_color(w.deep_color);
        wb2.add_reflection_color(w.reflection_color);
        wb2.add_sun_specular_power(w.sun_specular_power);
        wb2.add_reflectivity(w.reflectivity);
        wb2.add_fresnel(w.fresnel);
        wb2.add_fog_near(w.fog_near);
        wb2.add_fog_far(w.fog_far);
        wb2.add_specular_power(w.specular_power);
        wb2.add_refraction_magnitude(w.refraction_magnitude);
        wb2.add_reflection_magnitude(w.reflection_magnitude);
        wb2.add_layers(layers_off);
        wb2.add_noise(noise_off);
        waters.push_back(wb2.Finish());
        ++stats.waters;
    }
    return waters;
}

std::vector<flatbuffers::Offset<wfb::Climate>> EnvironmentCollector::write_climates(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::Climate>> climates;
    for (const auto& [id, c] : climates_) {
        std::vector<wfb::ClimateWeather> entries;
        for (const auto& [weather, chance] : c.weathers) {
            entries.emplace_back(weather, chance);
        }
        climates.push_back(wfb::CreateClimate(
            builder, c.id, builder.CreateString(c.editor_id),
            builder.CreateVectorOfStructs(entries), c.sun[0], c.sun[1], c.sun[2], c.sun[3],
            builder.CreateString(c.sun_texture), builder.CreateString(c.sun_glare_texture),
            builder.CreateString(c.sky), c.volatility, c.moons, c.phase_length));
        ++stats.climates;
    }
    return climates;
}

std::vector<flatbuffers::Offset<wfb::Weather>> EnvironmentCollector::write_weathers(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::Weather>> weathers;
    for (const auto& [id, w] : weathers_) {
        std::vector<flatbuffers::Offset<wfb::CloudLayer>> clouds;
        clouds.reserve(w.clouds.size());
        for (const auto& layer : w.clouds) {
            clouds.push_back(wfb::CreateCloudLayer(
                builder, builder.CreateString(layer.texture), layer.speed_x, layer.speed_y,
                builder.CreateVector(layer.colors.data(), layer.colors.size()),
                builder.CreateVector(layer.alphas.data(), layer.alphas.size()), layer.enabled));
        }
        weathers.push_back(wfb::CreateWeather(
            builder, w.id, builder.CreateString(w.editor_id), builder.CreateVector(w.colors),
            builder.CreateVector(w.fog), builder.CreateVector(w.directional_ambient),
            builder.CreateVector(clouds), w.wind_speed, w.wind_direction,
            w.wind_direction_range, w.transition_delta, w.sun_glare, w.sun_damage,
            w.precipitation_begin, w.precipitation_end, w.thunder_begin, w.thunder_end,
            w.thunder_frequency, w.classification, w.lightning_color, w.precipitation,
            builder.CreateString(w.aurora), builder.CreateVector(w.image_spaces)));
        ++stats.weathers;
    }
    return weathers;
}

std::vector<flatbuffers::Offset<wfb::ImageSpace>> EnvironmentCollector::write_image_spaces(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::ImageSpace>> image_spaces;
    for (const auto& [id, i] : image_spaces_) {
        image_spaces.push_back(wfb::CreateImageSpace(
            builder, i.id, builder.CreateString(i.editor_id), builder.CreateVector(i.hdr),
            builder.CreateVector(i.cinematic), builder.CreateVector(i.tint)));
        ++stats.image_spaces;
    }
    return image_spaces;
}

std::vector<flatbuffers::Offset<wfb::Precipitation>> EnvironmentCollector::write_precipitations(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::Precipitation>> precipitations;
    for (const auto& [id, p] : precipitations_) {
        precipitations.push_back(wfb::CreatePrecipitation(
            builder, p.id, builder.CreateString(p.editor_id), builder.CreateString(p.texture),
            p.gravity_velocity, p.rotation_velocity, p.size_x, p.size_y, p.center_offset_min,
            p.center_offset_max, p.rotation_range, p.subtextures_x, p.subtextures_y, p.type,
            p.box_size, p.density));
        ++stats.precipitations;
    }
    return precipitations;
}

std::vector<flatbuffers::Offset<wfb::Region>> EnvironmentCollector::write_regions(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::Region>> regions;
    for (const auto& [id, r] : regions_) {
        std::vector<flatbuffers::Offset<wfb::RegionArea>> areas;
        for (const auto& area : r.areas) {
            areas.push_back(wfb::CreateRegionArea(builder, builder.CreateVector(area)));
        }
        std::vector<wfb::RegionWeather> entries;
        for (const auto& e : r.weathers) {
            entries.emplace_back(e.weather, e.chance, e.global);
        }
        regions.push_back(wfb::CreateRegion(builder, r.id, builder.CreateString(r.editor_id),
                                            r.world, builder.CreateVector(areas),
                                            builder.CreateVectorOfStructs(entries),
                                            r.weather_priority, r.weather_override));
        ++stats.regions;
    }
    return regions;
}

} // namespace bethconv::pack::detail
