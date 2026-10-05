// SPDX-License-Identifier: GPL-3.0-or-later
#include "bases.hpp"

#include "fb_write.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/field_walk.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/forms_object.hpp"
#include "bethconv/record/forms_world.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace bethconv::pack::detail {
namespace {

using io::FourCC;

static_assert(same_bit(wfb::LightFlags::dynamic, record::Light::Flag::dynamic) &&
              same_bit(wfb::LightFlags::can_be_carried, record::Light::Flag::can_be_carried) &&
              same_bit(wfb::LightFlags::negative, record::Light::Flag::negative) &&
              same_bit(wfb::LightFlags::flicker, record::Light::Flag::flicker) &&
              same_bit(wfb::LightFlags::off_by_default, record::Light::Flag::off_by_default) &&
              same_bit(wfb::LightFlags::flicker_slow, record::Light::Flag::flicker_slow) &&
              same_bit(wfb::LightFlags::pulse, record::Light::Flag::pulse) &&
              same_bit(wfb::LightFlags::pulse_slow, record::Light::Flag::pulse_slow) &&
              same_bit(wfb::LightFlags::spot_light, record::Light::Flag::spot_light) &&
              same_bit(wfb::LightFlags::spot_shadow, record::Light::Flag::spot_shadow));

/// Types whose VMAD is followed by fragment data; their scripts only run
/// through systems that do not exist yet.
bool has_fragments(FourCC type) {
    return type == FourCC{"QUST"} || type == FourCC{"INFO"} || type == FourCC{"PACK"} ||
           type == FourCC{"SCEN"} || type == FourCC{"PERK"};
}

} // namespace

void BaseCollector::collect(const record::MergedRecord& merged, io::SpanReader& data,
                            const record::FormContext& form_ctx) {
    switch (merged.type.value) {
    case FourCC{"LIGH"}.value:
        on_light(merged, data, form_ctx);
        break;
    case FourCC{"MATO"}.value:
        on_material_object(merged, data);
        break;
    case FourCC{"ADDN"}.value:
        on_addon_node(merged, data);
        break;
    case FourCC{"TXST"}.value:
        on_texture_set(merged, data, form_ctx);
        break;
    case FourCC{"LTEX"}.value:
        on_land_texture(merged, data, form_ctx);
        break;
    case FourCC{"GRAS"}.value:
        on_grass(merged, data);
        break;
    default:
        break;
    }
}

void BaseCollector::on_light(const record::MergedRecord& merged, io::SpanReader& data,
                             const record::FormContext& form_ctx) {
    auto light = record::parse_light(data, form_ctx);
    if (!light) {
        ++shared_.stats().parse_errors;
        return;
    }
    bases_[merged.form.value] = BaseEntry{
        .id = merged.form.value,
        .type = merged.type.value,
        .editor_id = light->editor_id,
        .model = light->model.path.empty() ? std::string{} : model_vpath(light->model.path),
        .light =
            WorldLight{
                .radius = light->radius,
                .color = light->colour,
                .flags = static_cast<wfb::LightFlags>(light->light_flags),
                .falloff_exponent = light->falloff_exponent,
                .fov = light->fov,
                .near_clip = light->near_clip,
                .fade = light->fade,
                .flicker_period = light->flicker_period,
                .flicker_intensity = light->flicker_intensity_amplitude,
                .flicker_movement = light->flicker_movement_amplitude,
            },
        .flags = 0,
        .scripts = {},
        .record_flags = static_cast<wfb::RecordFlags>(merged.flags),
    };
    bool failed = false;
    bases_[merged.form.value].scripts =
        shared_.global_scripts(merged, light->scripts, failed);
    if (failed) {
        ++shared_.stats().unresolved;
    }
}

/// Any other type: keep it as a base if it has a model or scripts. ARMO's
/// MODL is an armature FormID, not a path; its world model is MOD2 (male)
/// or MOD4 (female). Source: UESP, ARMO record.
void BaseCollector::collect_generic(const record::MergedRecord& merged, io::SpanReader& data) {
    const bool armor = merged.type == FourCC{"ARMO"};
    const bool door = merged.type == FourCC{"DOOR"};
    const bool scripted = !has_fragments(merged.type);
    std::string editor_id;
    std::string modl;
    std::string mod2;
    std::string mod4;
    std::uint32_t flags = 0;
    record::ScriptData scripts;
    const bool stat = merged.type == FourCC{"STAT"};
    float max_angle = 0.0F;
    std::uint32_t material = 0;
    const auto walked = record::for_each_field(
        data, [&](const record::FieldHeader& field, io::SpanReader& body) {
            if (field.type == FourCC{"DNAM"} && stat && body.remaining() >= 8) {
                max_angle = body.get<float>().value_or(0.0F);
                material = body.get<std::uint32_t>().value_or(0);
            } else if (field.type == FourCC{"EDID"} && editor_id.empty()) {
                editor_id = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"MODL"} && modl.empty() && !armor) {
                modl = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"MOD2"} && mod2.empty()) {
                mod2 = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"MOD4"} && mod4.empty() && armor) {
                mod4 = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"FNAM"} && door) {
                flags = body.get<std::uint8_t>().value_or(0);
            } else if (field.type == FourCC{"VMAD"} && scripted) {
                if (auto read = record::read_script_data(body)) {
                    scripts = std::move(*read);
                } else {
                    ++shared_.stats().script_errors;
                }
            }
        });
    if (!walked) {
        ++shared_.stats().parse_errors;
        return;
    }
    const std::string& path = !modl.empty() ? modl : !mod2.empty() ? mod2 : mod4;
    if (path.empty() && scripts.empty()) {
        return;
    }
    bool failed = false;
    bases_[merged.form.value] = BaseEntry{
        .id = merged.form.value,
        .type = merged.type.value,
        .editor_id = std::move(editor_id),
        .model = path.empty() ? std::string{} : model_vpath(path),
        .light = std::nullopt,
        .flags = flags,
        .scripts = shared_.global_scripts(merged, scripts, failed),
        .record_flags = static_cast<wfb::RecordFlags>(merged.flags),
        .directional_material =
            material != 0 ? shared_.global(merged, record::FormId{material}, failed) : 0,
        .directional_max_angle = max_angle,
    };
    if (failed) {
        ++shared_.stats().unresolved;
    }
}

void BaseCollector::on_material_object(const record::MergedRecord& merged, io::SpanReader& data) {
    MaterialObjectEntry out;
    out.id = merged.form.value;
    const auto walked = record::for_each_field(
        data, [&](const record::FieldHeader& field, io::SpanReader& body) {
            if (field.type == FourCC{"EDID"}) {
                out.editor_id = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"MODL"}) {
                out.model = model_vpath(std::string(body.zstring().value_or("")));
            } else if (field.type == FourCC{"DATA"}) {
                for (auto& f : out.data) {
                    f = body.remaining() >= 4 ? body.get<float>().value_or(0.0F) : 0.0F;
                }
                out.flags = body.remaining() >= 4 ? body.get<std::uint32_t>().value_or(0) : 0;
            }
        });
    if (!walked) {
        ++shared_.stats().parse_errors;
        return;
    }
    material_objects_[out.id] = std::move(out);
}

void BaseCollector::on_land_texture(const record::MergedRecord& merged, io::SpanReader& data,
                                    const record::FormContext& form_ctx) {
    auto ltex = record::parse_land_texture(data, form_ctx);
    if (!ltex) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    std::vector<std::uint32_t> grasses;
    for (const auto grass : ltex->grasses) {
        grasses.push_back(shared_.global(merged, grass, failed));
    }
    land_textures_[merged.form.value] = LandTextureEntry{
        .id = merged.form.value,
        .editor_id = ltex->editor_id,
        .texture_set = shared_.global(merged, ltex->texture_set, failed),
        .specular = ltex->specular,
        .grasses = std::move(grasses),
    };
    if (failed) {
        ++shared_.stats().unresolved;
    }
}

void BaseCollector::on_addon_node(const record::MergedRecord& merged, io::SpanReader& data) {
    AddonEntry out;
    out.id = merged.form.value;
    bool has_index = false;
    const auto walked = record::for_each_field(
        data, [&](const record::FieldHeader& field, io::SpanReader& body) {
            if (field.type == FourCC{"EDID"}) {
                out.editor_id = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"MODL"}) {
                out.model = model_vpath(std::string(body.zstring().value_or("")));
            } else if (field.type == FourCC{"DATA"} && body.remaining() >= 4) {
                out.index = body.get<std::int32_t>().value_or(0);
                has_index = true;
            }
        });
    if (!walked || !has_index || out.model.empty()) {
        ++shared_.stats().parse_errors;
        return;
    }
    addons_[out.id] = std::move(out);
}

void BaseCollector::on_grass(const record::MergedRecord& merged, io::SpanReader& data) {
    GrassEntry out;
    out.id = merged.form.value;
    const auto walked = record::for_each_field(
        data, [&](const record::FieldHeader& field, io::SpanReader& body) {
            if (field.type == FourCC{"EDID"}) {
                out.editor_id = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"MODL"}) {
                out.model = model_vpath(std::string(body.zstring().value_or("")));
            } else if (field.type == FourCC{"DATA"} && body.remaining() >= 29) {
                out.density = body.get<std::uint8_t>().value_or(0);
                out.min_slope = body.get<std::uint8_t>().value_or(0);
                out.max_slope = body.get<std::uint8_t>().value_or(90);
                (void)body.skip(1);
                out.units_from_water = body.get<std::uint16_t>().value_or(0);
                (void)body.skip(2);
                out.water_type = body.get<std::uint32_t>().value_or(0);
                out.position_range = body.get<float>().value_or(0.0F);
                out.height_range = body.get<float>().value_or(0.0F);
                out.color_range = body.get<float>().value_or(0.0F);
                out.wave_period = body.get<float>().value_or(0.0F);
                out.flags = static_cast<wfb::GrassFlags>(body.get<std::uint8_t>().value_or(0));
            }
        });
    if (!walked || out.model.empty()) {
        ++shared_.stats().parse_errors;
        return;
    }
    grasses_[out.id] = std::move(out);
}

void BaseCollector::on_texture_set(const record::MergedRecord& merged, io::SpanReader& data,
                                   const record::FormContext& form_ctx) {
    auto txst = record::parse_texture_set(data, form_ctx);
    if (!txst) {
        ++shared_.stats().parse_errors;
        return;
    }
    texture_sets_[merged.form.value] = TextureSetEntry{
        .diffuse = texture_vpath(txst->textures[0]),
        .normal = texture_vpath(txst->textures[1]),
    };
}

std::vector<flatbuffers::Offset<wfb::Base>> BaseCollector::write_bases(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::Base>> bases;
    bases.reserve(bases_.size());
    for (const auto& [id, base] : bases_) {
        const auto editor_id = builder.CreateString(base.editor_id);
        const auto model = builder.CreateString(base.model);
        const auto scripts = base.scripts.empty() ? 0 : write_scripts(builder, base.scripts);
        stats.scripts += base.scripts.size();
        std::optional<wfb::LightData> light;
        if (base.light) {
            const auto& l = *base.light;
            light = wfb::LightData(l.radius, l.color, l.flags, l.falloff_exponent, l.fov,
                                   l.near_clip, l.fade, l.flicker_period, l.flicker_intensity,
                                   l.flicker_movement);
        }
        wfb::BaseBuilder bb(builder);
        bb.add_id(base.id);
        bb.add_type(base.type);
        bb.add_editor_id(editor_id);
        bb.add_model(model);
        bb.add_flags(base.flags);
        bb.add_record_flags(base.record_flags);
        if (base.directional_material != 0) {
            bb.add_directional_material(base.directional_material);
            bb.add_directional_max_angle(base.directional_max_angle);
        }
        if (!base.scripts.empty()) {
            bb.add_scripts(scripts);
        }
        if (light) {
            bb.add_has_light(true);
            bb.add_light(&*light);
            ++stats.lights;
        }
        bases.push_back(bb.Finish());
        ++stats.bases;
    }
    return bases;
}

std::vector<flatbuffers::Offset<wfb::LandTexture>> BaseCollector::write_land_textures(
    flatbuffers::FlatBufferBuilder& builder) {
    auto& stats = shared_.stats();
    std::vector<flatbuffers::Offset<wfb::LandTexture>> land_textures;
    for (const auto& [id, ltex] : land_textures_) {
        TextureSetEntry paths;
        if (const auto t = texture_sets_.find(ltex.texture_set);
            t != texture_sets_.end()) {
            paths = t->second;
        }
        land_textures.push_back(wfb::CreateLandTexture(
            builder, ltex.id, builder.CreateString(ltex.editor_id),
            builder.CreateString(paths.diffuse), builder.CreateString(paths.normal),
            ltex.specular, builder.CreateVector(ltex.grasses)));
        ++stats.land_textures;
    }
    return land_textures;
}

std::vector<flatbuffers::Offset<wfb::MaterialObject>> BaseCollector::write_material_objects(
    flatbuffers::FlatBufferBuilder& builder) {
    std::vector<flatbuffers::Offset<wfb::MaterialObject>> material_objects;
    for (const auto& [id, m] : material_objects_) {
        const auto& d = m.data;
        const std::array<float, 3> projection{d[4], d[5], d[6]};
        const std::array<float, 3> colour{d[8], d[9], d[10]};
        material_objects.push_back(wfb::CreateMaterialObject(
            builder, m.id, builder.CreateString(m.editor_id), builder.CreateString(m.model), d[0], d[1],
            d[2], d[3], builder.CreateVector(projection.data(), projection.size()), d[7],
            builder.CreateVector(colour.data(), colour.size()), (m.flags & 1U) != 0));
    }
    return material_objects;
}

std::vector<flatbuffers::Offset<wfb::Grass>> BaseCollector::write_grasses(
    flatbuffers::FlatBufferBuilder& builder) {
    std::vector<flatbuffers::Offset<wfb::Grass>> grasses;
    for (const auto& [id, g] : grasses_) {
        grasses.push_back(wfb::CreateGrass(builder, g.id, builder.CreateString(g.editor_id),
                                           builder.CreateString(g.model), g.density, g.min_slope,
                                           g.max_slope, g.units_from_water, g.water_type, g.position_range,
                                           g.height_range, g.color_range, g.wave_period, g.flags));
    }
    return grasses;
}

std::vector<flatbuffers::Offset<wfb::AddonNode>> BaseCollector::write_addon_nodes(
    flatbuffers::FlatBufferBuilder& builder) {
    std::vector<flatbuffers::Offset<wfb::AddonNode>> addon_nodes;
    for (const auto& [id, a] : addons_) {
        addon_nodes.push_back(wfb::CreateAddonNode(builder, a.id, builder.CreateString(a.editor_id), a.index,
                                                   builder.CreateString(a.model)));
    }
    return addon_nodes;
}

} // namespace bethconv::pack::detail
