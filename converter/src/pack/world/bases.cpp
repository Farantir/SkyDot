// SPDX-License-Identifier: GPL-3.0-or-later
#include "bases.hpp"

#include "fb_write.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/field_walk.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/forms_object.hpp"
#include "bethconv/record/forms_world.hpp"

#include <algorithm>
#include <array>
#include <memory>
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
    bool failed = false;
    wfb::BaseT out;
    out.id = merged.form.value;
    out.type = merged.type.value;
    out.editor_id = light->editor_id;
    out.model = light->model.path.empty() ? std::string{} : model_vpath(light->model.path);
    out.has_light = true;
    out.light = std::make_unique<wfb::LightData>(
        light->radius, light->colour, static_cast<wfb::LightFlags>(light->light_flags),
        light->falloff_exponent, light->fov, light->near_clip, light->fade, light->flicker_period,
        light->flicker_intensity_amplitude, light->flicker_movement_amplitude);
    out.scripts = shared_.global_scripts(merged, light->scripts, failed);
    out.record_flags = static_cast<wfb::RecordFlags>(merged.flags);
    if (failed) {
        ++shared_.stats().unresolved;
    }
    bases_[out.id] = std::move(out);
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
    wfb::BaseT out;
    out.id = merged.form.value;
    out.type = merged.type.value;
    out.editor_id = std::move(editor_id);
    out.model = path.empty() ? std::string{} : model_vpath(path);
    out.flags = flags;
    out.scripts = shared_.global_scripts(merged, scripts, failed);
    out.record_flags = static_cast<wfb::RecordFlags>(merged.flags);
    if (material != 0) {
        out.directional_material = shared_.global(merged, record::FormId{material}, failed);
    }
    if (out.directional_material != 0) {
        out.directional_max_angle = max_angle;
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    bases_[out.id] = std::move(out);
}

void BaseCollector::on_material_object(const record::MergedRecord& merged, io::SpanReader& data) {
    wfb::MaterialObjectT out;
    out.id = merged.form.value;
    out.projection.assign(3, 0.0F);
    out.single_pass_color.assign(3, 0.0F);
    const auto walked = record::for_each_field(
        data, [&](const record::FieldHeader& field, io::SpanReader& body) {
            if (field.type == FourCC{"EDID"}) {
                out.editor_id = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"MODL"}) {
                out.model = model_vpath(std::string(body.zstring().value_or("")));
            } else if (field.type == FourCC{"DATA"}) {
                std::array<float, 11> d{};
                for (auto& f : d) {
                    f = body.remaining() >= 4 ? body.get<float>().value_or(0.0F) : 0.0F;
                }
                const std::uint32_t flags =
                    body.remaining() >= 4 ? body.get<std::uint32_t>().value_or(0) : 0;
                out.falloff_scale = d[0];
                out.falloff_bias = d[1];
                out.noise_uv_scale = d[2];
                out.material_uv_scale = d[3];
                out.projection = {d[4], d[5], d[6]};
                out.normal_dampener = d[7];
                out.single_pass_color = {d[8], d[9], d[10]};
                out.single_pass = (flags & 1U) != 0;
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
    LandTextureEntry out;
    out.table.id = merged.form.value;
    out.table.editor_id = ltex->editor_id;
    out.table.specular = ltex->specular;
    out.table.grasses = std::move(grasses);
    out.texture_set = shared_.global(merged, ltex->texture_set, failed);
    land_textures_[merged.form.value] = std::move(out);
    if (failed) {
        ++shared_.stats().unresolved;
    }
}

void BaseCollector::on_addon_node(const record::MergedRecord& merged, io::SpanReader& data) {
    wfb::AddonNodeT out;
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
    wfb::GrassT out;
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

void BaseCollector::finish(wfb::WorldT& world) {
    auto& stats = shared_.stats();
    for (const auto& [id, base] : bases_) {
        stats.scripts += base.scripts.size();
        if (base.has_light) {
            ++stats.lights;
        }
    }
    stats.bases += bases_.size();
    stats.land_textures += land_textures_.size();
    move_into(world.bases, bases_);
    for (auto& [id, ltex] : land_textures_) {
        if (const auto t = texture_sets_.find(ltex.texture_set); t != texture_sets_.end()) {
            ltex.table.diffuse = t->second.diffuse;
            ltex.table.normal = t->second.normal;
        }
        world.land_textures.push_back(std::make_unique<wfb::LandTextureT>(std::move(ltex.table)));
    }
    move_into(world.material_objects, material_objects_);
    move_into(world.grasses, grasses_);
    move_into(world.addon_nodes, addons_);
}

} // namespace bethconv::pack::detail
