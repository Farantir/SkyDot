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
#include <cmath>
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
    case FourCC{"KYWD"}.value:
        on_keyword(merged, data);
        break;
    case FourCC{"MESG"}.value:
        on_message(merged, data, form_ctx);
        break;
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
    case FourCC{"AMMO"}.value:
        on_ammo(merged, data, form_ctx);
        break;
    case FourCC{"PROJ"}.value:
        on_projectile(merged, data, form_ctx);
        break;
    case FourCC{"SNDR"}.value:
        on_sound_descriptor(merged, data, form_ctx);
        break;
    case FourCC{"SOUN"}.value:
        on_sound_marker(merged, data, form_ctx);
        break;
    case FourCC{"SOPM"}.value:
        on_sound_output_model(merged, data);
        break;
    case FourCC{"EXPL"}.value:
        on_explosion(merged, data);
        break;
    default:
        break;
    }
}

void BaseCollector::keep_alternates(const record::MergedRecord& merged,
                                    std::span<const std::byte> mods) {
    if (mods.empty()) {
        return;
    }
    io::SpanReader in(mods, "MODS");
    const auto count = in.get<std::uint32_t>();
    if (!count) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    std::vector<PendingAlternate> list;
    for (std::uint32_t i = 0; i < *count; ++i) {
        const auto length = in.get<std::uint32_t>();
        if (!length || *length > in.remaining()) {
            ++shared_.stats().parse_errors;
            break;
        }
        PendingAlternate entry;
        if (const auto name = in.chars(*length)) {
            entry.shape.assign(name->data(), name->size());
        }
        const auto txst = in.get<std::uint32_t>();
        const auto index = in.get<std::int32_t>();
        if (!txst || !index) {
            ++shared_.stats().parse_errors;
            break;
        }
        entry.index = *index;
        entry.texture_set = shared_.global(merged, record::FormId{*txst}, failed);
        list.push_back(std::move(entry));
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    if (!list.empty()) {
        pending_alternates_[merged.form.value] = std::move(list);
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
    keep_alternates(merged, light->model.alternate_textures);
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
    std::vector<std::byte> mods;
    std::uint32_t flags = 0;
    record::ScriptData scripts;
    const bool stat = merged.type == FourCC{"STAT"};
    float max_angle = 0.0F;
    std::uint32_t material = 0;
    record::KeywordList keywords;
    std::optional<io::ParseError> keyword_failure;
    record::Destruction destruction;
    const auto walked = record::for_each_field(
        data, [&](const record::FieldHeader& field, io::SpanReader& body) {
            if (field.type == FourCC{"DNAM"} && stat && body.remaining() >= 8) {
                max_angle = body.get<float>().value_or(0.0F);
                material = body.get<std::uint32_t>().value_or(0);
            } else if (field.type == FourCC{"EDID"} && editor_id.empty()) {
                editor_id = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"MODL"} && modl.empty() && !armor) {
                modl = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"MODS"} && mods.empty()) {
                if (const auto raw = body.bytes(body.remaining())) {
                    mods.assign(raw->begin(), raw->end());
                }
            } else if (field.type == FourCC{"MOD2"} && mod2.empty()) {
                mod2 = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"MOD4"} && mod4.empty() && armor) {
                mod4 = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"FNAM"} && door) {
                flags = body.get<std::uint8_t>().value_or(0);
            } else if (record::read_keyword_field(keywords, field, body, keyword_failure)) {
                // KSIZ and KWDA
            } else if (record::read_destruction_field(destruction, field, body, keyword_failure)) {
                // DEST, DSTD, DMDL...
            } else if (field.type == FourCC{"VMAD"} && scripted) {
                if (auto read = record::read_script_data(body)) {
                    scripts = std::move(*read);
                } else {
                    ++shared_.stats().script_errors;
                }
            }
        });
    if (!walked || keyword_failure) {
        ++shared_.stats().parse_errors;
        return;
    }
    const std::string& path = !modl.empty() ? modl : !mod2.empty() ? mod2 : mod4;
    // A sound marker has no model but is a base, so its placed references resolve.
    if (path.empty() && scripts.empty() && merged.type != FourCC{"SOUN"}) {
        return;
    }
    bool failed = false;
    wfb::BaseT out;
    out.id = merged.form.value;
    out.type = merged.type.value;
    out.editor_id = std::move(editor_id);
    out.model = path.empty() ? std::string{} : model_vpath(path);
    if (!armor && !modl.empty()) {
        keep_alternates(merged, mods);
    }
    out.flags = flags;
    out.keywords = shared_.global_all(merged, keywords.keywords, failed);
    if (!destruction.empty()) {
        auto dest = std::make_unique<wfb::DestructionT>();
        dest->health = destruction.health;
        dest->flags = destruction.flags;
        for (const auto& stage : destruction.stages) {
            auto st = std::make_unique<wfb::DestructionStageT>();
            st->health_percent = stage.health_percent;
            st->index = stage.index;
            st->damage_stage = stage.damage_stage;
            st->flags = stage.stage_flags;
            st->self_damage_per_second = stage.self_damage_per_second;
            st->explosion = shared_.global(merged, stage.explosion, failed);
            st->debris = shared_.global(merged, stage.debris, failed);
            st->debris_count = stage.debris_count;
            if (!stage.model.path.empty()) {
                st->model = model_vpath(stage.model.path);
            }
            dest->stages.push_back(std::move(st));
        }
        out.destruction = std::move(dest);
    }
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

void BaseCollector::on_ammo(const record::MergedRecord& merged, io::SpanReader& data,
                            const record::FormContext& form_ctx) {
    auto ammo = record::parse_ammo(data, form_ctx);
    if (!ammo) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    wfb::AmmoT out;
    out.id = merged.form.value;
    out.projectile = shared_.global(merged, ammo->projectile, failed);
    out.flags = ammo->flags;
    if (failed) {
        ++shared_.stats().unresolved;
    }
    ammos_[out.id] = std::move(out);
}

void BaseCollector::on_projectile(const record::MergedRecord& merged, io::SpanReader& data,
                                  const record::FormContext& form_ctx) {
    auto projectile = record::parse_projectile(data, form_ctx);
    const auto flight = projectile ? record::projectile_flight(projectile->data) : std::nullopt;
    if (!flight) {
        ++shared_.stats().parse_errors;
        return;
    }
    wfb::ProjectileT out;
    out.id = merged.form.value;
    out.type = flight->type;
    out.flags = flight->flags;
    out.gravity = flight->gravity;
    out.speed = flight->speed;
    out.range = flight->range;
    out.collision_radius = flight->collision_radius;
    out.lifetime = flight->lifetime;
    out.muzzle_flash_duration = flight->muzzle_flash_duration;
    if (!projectile->muzzle_flash_model.empty()) {
        out.muzzle_flash_model = model_vpath(projectile->muzzle_flash_model);
    }
    projectiles_[out.id] = std::move(out);
}

/// SNDR. Field layouts checked against Skyrim.esm: ANAM file names (relative
/// to Data\\Sound, with or without that prefix), CNAM type, GNAM category,
/// ONAM output model, LNAM 4 bytes (byte 1 looping), BNAM 6 bytes.
void BaseCollector::on_sound_descriptor(const record::MergedRecord& merged, io::SpanReader& data,
                                        const record::FormContext&) {
    wfb::SoundDescriptorT out;
    out.id = merged.form.value;
    bool failed = false;
    const auto walked = record::for_each_field(
        data, [&](const record::FieldHeader& field, io::SpanReader& body) {
            if (field.type == FourCC{"EDID"}) {
                out.editor_id = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"ANAM"}) {
                if (auto path = sound_vpath(body.zstring().value_or("")); !path.empty()) {
                    out.files.push_back(std::move(path));
                }
            } else if (field.type == FourCC{"CNAM"}) {
                out.descriptor_type = body.get<std::uint32_t>().value_or(0);
            } else if (field.type == FourCC{"GNAM"}) {
                out.category = shared_.global(merged, record::FormId{body.get<std::uint32_t>().value_or(0)}, failed);
            } else if (field.type == FourCC{"ONAM"}) {
                out.output_model = shared_.global(merged, record::FormId{body.get<std::uint32_t>().value_or(0)}, failed);
            } else if (field.type == FourCC{"LNAM"} && body.remaining() >= 2) {
                (void)body.get<std::uint8_t>();
                out.looping = body.get<std::uint8_t>().value_or(0);
            } else if (field.type == FourCC{"BNAM"} && body.remaining() >= 6) {
                out.frequency_shift = static_cast<std::int8_t>(body.get<std::uint8_t>().value_or(0));
                out.frequency_variance = static_cast<std::int8_t>(body.get<std::uint8_t>().value_or(0));
                out.priority = body.get<std::uint8_t>().value_or(0);
                out.db_variance = body.get<std::uint8_t>().value_or(0);
                out.static_attenuation = body.get<std::uint16_t>().value_or(0);
            }
        });
    if (!walked) {
        ++shared_.stats().parse_errors;
        return;
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    sound_descriptors_[out.id] = std::move(out);
}

/// SOUN: EDID and SDSC. The old inline sound (FNAM, SNDD) is not read.
void BaseCollector::on_sound_marker(const record::MergedRecord& merged, io::SpanReader& data,
                                    const record::FormContext&) {
    wfb::SoundMarkerT out;
    out.id = merged.form.value;
    bool failed = false;
    const auto walked = record::for_each_field(
        data, [&](const record::FieldHeader& field, io::SpanReader& body) {
            if (field.type == FourCC{"EDID"}) {
                out.editor_id = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"SDSC"}) {
                out.descriptor = shared_.global(merged, record::FormId{body.get<std::uint32_t>().value_or(0)}, failed);
            }
        });
    if (!walked) {
        ++shared_.stats().parse_errors;
        return;
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    sound_markers_[out.id] = std::move(out);
}

/// EXPL: MODL and DATA (52 bytes: light, two sounds, impact data set, placed
/// object, projectile, then force, damage, radius, image space radius,
/// vertical offset multiplier floats, flags and sound level).
void BaseCollector::on_explosion(const record::MergedRecord& merged, io::SpanReader& data) {
    wfb::ExplosionT out;
    out.id = merged.form.value;
    bool failed = false;
    const auto walked = record::for_each_field(
        data, [&](const record::FieldHeader& field, io::SpanReader& body) {
            if (field.type == FourCC{"EDID"}) {
                out.editor_id = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"MODL"} && out.model.empty()) {
                const auto path = std::string(body.zstring().value_or(""));
                out.model = path.empty() ? std::string{} : model_vpath(path);
            } else if (field.type == FourCC{"DATA"} && body.remaining() >= 48) {
                const auto form = [&] {
                    return shared_.global(merged, record::FormId{body.get<std::uint32_t>().value_or(0)}, failed);
                };
                out.light = form();
                out.sound = form();
                out.sound_far = form();
                out.impact_data = form();
                out.placed_object = form();
                out.projectile = form();
                out.force = body.get<float>().value_or(0.0F);
                out.damage = body.get<float>().value_or(0.0F);
                out.radius = body.get<float>().value_or(0.0F);
                (void)body.skip(8); // image space radius, vertical offset multiplier
                if (body.remaining() >= 4) {
                    out.flags = body.get<std::uint32_t>().value_or(0);
                }
            }
        });
    if (!walked) {
        ++shared_.stats().parse_errors;
        return;
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    explosions_[out.id] = std::move(out);
}

/// KYWD: only the editor id, which `Keyword.GetKeyword` looks up.
void BaseCollector::on_keyword(const record::MergedRecord& merged, io::SpanReader& data) {
    wfb::KeywordT out;
    out.id = merged.form.value;
    const auto walked = record::for_each_field(
        data, [&](const record::FieldHeader& field, io::SpanReader& body) {
            if (field.type == FourCC{"EDID"}) {
                out.editor_id = std::string(body.zstring().value_or(""));
            }
        });
    if (!walked) {
        ++shared_.stats().parse_errors;
        return;
    }
    keywords_[out.id] = std::move(out);
}

/// MESG: FULL title, DESC text, DNAM flags (bit 0 message box, bit 1 auto
/// display), ITXT one per button. Display time, icon, quest and the buttons'
/// conditions are left out.
void BaseCollector::on_message(const record::MergedRecord& merged, io::SpanReader& data,
                               const record::FormContext& form_ctx) {
    wfb::MessageT out;
    out.id = merged.form.value;
    std::optional<io::ParseError> failure;
    const auto text = [&](const record::FieldHeader& field, io::SpanReader& body,
                          record::StringKind kind) {
        auto read = record::read_lstring(body, form_ctx, FourCC{"MESG"}, field.type, kind);
        if (!read) {
            if (!failure) {
                failure = std::move(read).error();
            }
            return std::string{};
        }
        return read->text;
    };
    const auto walked = record::for_each_field(
        data, [&](const record::FieldHeader& field, io::SpanReader& body) {
            if (field.type == FourCC{"EDID"}) {
                out.editor_id = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"FULL"}) {
                out.title = text(field, body, record::StringKind::plain);
            } else if (field.type == FourCC{"DESC"}) {
                out.text = text(field, body, record::StringKind::description);
            } else if (field.type == FourCC{"ITXT"}) {
                out.buttons.push_back(text(field, body, record::StringKind::plain));
            } else if (field.type == FourCC{"DNAM"}) {
                out.flags = static_cast<wfb::MessageFlags>(body.get<std::uint32_t>().value_or(0));
            }
        });
    if (!walked || failure) {
        ++shared_.stats().parse_errors;
        return;
    }
    messages_[out.id] = std::move(out);
}

/// SOPM: NAM1 (byte 0 flags), MNAM type, ANAM 20 bytes: 4 reserved, min and
/// max distance floats, 5 curve bytes, 3 reserved.
void BaseCollector::on_sound_output_model(const record::MergedRecord& merged, io::SpanReader& data) {
    wfb::SoundOutputModelT out;
    out.id = merged.form.value;
    const auto walked = record::for_each_field(
        data, [&](const record::FieldHeader& field, io::SpanReader& body) {
            if (field.type == FourCC{"EDID"}) {
                out.editor_id = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"NAM1"}) {
                out.flags = body.get<std::uint8_t>().value_or(0);
            } else if (field.type == FourCC{"MNAM"}) {
                out.type = body.get<std::uint32_t>().value_or(0);
            } else if (field.type == FourCC{"ANAM"} && body.remaining() >= 20) {
                (void)body.skip(4);
                out.min_distance = body.get<float>().value_or(0.0F);
                out.max_distance = body.get<float>().value_or(0.0F);
                for (int i = 0; i < 5; ++i) {
                    out.curve.push_back(body.get<std::uint8_t>().value_or(0));
                }
            }
        });
    if (!walked) {
        ++shared_.stats().parse_errors;
        return;
    }
    sound_output_models_[out.id] = std::move(out);
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
    out.table.snow = (ltex->inam & 1U) != 0;
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
            } else if (field.type == FourCC{"OBND"} && body.remaining() >= 12) {
                std::array<float, 6> b{};
                for (float& v : b) {
                    v = static_cast<float>(body.get<std::int16_t>().value_or(0));
                }
                const float dx = b[3] - b[0];
                const float dy = b[4] - b[1];
                const float dz = b[5] - b[2];
                out.bound_radius = 0.5F * std::sqrt(dx * dx + dy * dy + dz * dz);
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
    TextureSetEntry entry;
    entry.diffuse = texture_vpath(txst->textures[0]);
    entry.normal = texture_vpath(txst->textures[1]);
    for (std::size_t i = 0; i < entry.textures.size(); ++i) {
        if (!txst->textures[i].empty()) {
            entry.textures[i] = texture_vpath(txst->textures[i]);
        }
    }
    texture_sets_[merged.form.value] = std::move(entry);
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
    stats.ammos += ammos_.size();
    stats.projectiles += projectiles_.size();
    stats.sound_descriptors += sound_descriptors_.size();
    stats.sound_markers += sound_markers_.size();
    stats.sound_output_models += sound_output_models_.size();
    stats.explosions += explosions_.size();
    stats.keywords += keywords_.size();
    stats.messages += messages_.size();
    stats.land_textures += land_textures_.size();
    for (auto& [id, list] : pending_alternates_) {
        const auto base = bases_.find(id);
        if (base == bases_.end()) {
            continue;
        }
        for (const auto& pending : list) {
            const auto set = texture_sets_.find(pending.texture_set);
            if (set == texture_sets_.end()) {
                continue;
            }
            auto alt = std::make_unique<wfb::AlternateTextureT>();
            alt->shape = pending.shape;
            alt->index = pending.index;
            alt->textures.assign(set->second.textures.begin(), set->second.textures.end());
            base->second.alternate_textures.push_back(std::move(alt));
        }
    }
    move_into(world.bases, bases_);
    move_into(world.ammos, ammos_);
    move_into(world.projectiles, projectiles_);
    move_into(world.sound_descriptors, sound_descriptors_);
    move_into(world.sound_markers, sound_markers_);
    move_into(world.sound_output_models, sound_output_models_);
    move_into(world.explosions, explosions_);
    move_into(world.keywords, keywords_);
    move_into(world.messages, messages_);
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
