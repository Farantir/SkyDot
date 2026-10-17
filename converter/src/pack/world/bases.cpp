// SPDX-License-Identifier: GPL-3.0-or-later
#include "bases.hpp"

#include "fb_write.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/record/field_walk.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/forms_actor.hpp"
#include "bethconv/record/forms_game.hpp"
#include "bethconv/record/forms_object.hpp"
#include "bethconv/record/forms_world.hpp"

#include "skydot_formats/vpath.hpp"

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
    case FourCC{"COBJ"}.value:
        on_recipe(merged, data, form_ctx);
        break;
    case FourCC{"MGEF"}.value:
        on_magic_effect(merged, data, form_ctx);
        break;
    case FourCC{"ENCH"}.value:
        on_enchantment(merged, data, form_ctx);
        break;
    case FourCC{"GMST"}.value:
        on_game_setting(merged, data, form_ctx);
        break;
    case FourCC{"IPDS"}.value:
        on_impact_data_set(merged, data, form_ctx);
        break;
    case FourCC{"IPCT"}.value:
        on_impact(merged, data, form_ctx);
        break;
    case FourCC{"CSTY"}.value:
        on_combat_style(merged, data, form_ctx);
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
    if (light->has(record::Light::Flag::can_be_carried)) {
        out.name = light->name.text;
        out.item = std::make_unique<wfb::ItemDataT>();
        out.item->category = wfb::ItemCategory::light;
        out.item->weight = light->weight;
        out.item->value = static_cast<std::int32_t>(light->value);
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    bases_[out.id] = std::move(out);
}

/// Any other type: keep it as a base if it has a model or scripts. ARMO's
/// MODL is an armature FormID, not a path; its world model is MOD2 (male)
/// or MOD4 (female). Source: UESP, ARMO record.
void BaseCollector::collect_generic(const record::MergedRecord& merged, io::SpanReader& data,
                                    const record::FormContext& form_ctx) {
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
    record::LString name;
    record::LString activate_text;
    const bool has_activate_text = merged.type == FourCC{"ACTI"} || merged.type == FourCC{"FLOR"};
    std::optional<io::ParseError> text_failure;
    const auto walked = record::for_each_field(
        data, [&](const record::FieldHeader& field, io::SpanReader& body) {
            if (field.type == FourCC{"DNAM"} && stat && body.remaining() >= 8) {
                max_angle = body.get<float>().value_or(0.0F);
                material = body.get<std::uint32_t>().value_or(0);
            } else if (field.type == FourCC{"EDID"} && editor_id.empty()) {
                editor_id = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"MODL"} && modl.empty() && !armor) {
                modl = std::string(body.zstring().value_or(""));
            } else if (field.type == FourCC{"FULL"} && name.empty()) {
                if (auto read = record::read_lstring(body, form_ctx, merged.type, field.type)) {
                    name = std::move(*read);
                } else {
                    text_failure = read.error();
                }
            } else if (field.type == FourCC{"RNAM"} && has_activate_text) {
                if (auto read = record::read_lstring(body, form_ctx, merged.type, field.type)) {
                    activate_text = std::move(*read);
                } else {
                    text_failure = read.error();
                }
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
    if (!walked || keyword_failure || text_failure) {
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
    out.name = name.text;
    out.activate_text = activate_text.text;
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

namespace {

/// EFID/EFIT entries as world.fb's structs, the effects' FormIDs global.
std::vector<wfb::EffectItem> effect_items(CollectContext& shared,
                                          const record::MergedRecord& merged,
                                          const std::vector<record::EffectItem>& effects,
                                          bool& failed) {
    std::vector<wfb::EffectItem> out;
    out.reserve(effects.size());
    for (const auto& e : effects) {
        out.emplace_back(shared.global(merged, e.effect, failed), e.magnitude, e.area, e.duration);
    }
    return out;
}

} // namespace

bool BaseCollector::is_work_type(io::FourCC type) noexcept {
    return type == FourCC{"FLOR"} || type == FourCC{"TREE"} || type == FourCC{"FURN"};
}

void BaseCollector::collect_work(const record::MergedRecord& merged, io::SpanReader& data,
                                 const record::FormContext& form_ctx) {
    WorkEntry entry;
    entry.type = merged.type.value;
    entry.record_flags = merged.flags;
    bool failed = false;
    const auto pack_yield = [](const std::array<std::uint8_t, 4>& y) {
        return static_cast<std::uint32_t>(y[0]) | (static_cast<std::uint32_t>(y[1]) << 8U) |
               (static_cast<std::uint32_t>(y[2]) << 16U) | (static_cast<std::uint32_t>(y[3]) << 24U);
    };
    if (merged.type == FourCC{"FURN"}) {
        auto r = record::parse_furniture(data, form_ctx);
        if (!r) {
            ++shared_.stats().parse_errors;
            return;
        }
        entry.editor_id = r->editor_id;
        entry.bench_type = r->bench_type;
        entry.bench_skill = r->bench_skill;
        if (entry.bench_type == 0) {
            return;
        }
    } else {
        std::string editor_id;
        record::FormId ingredient;
        record::FormId sound;
        std::array<std::uint8_t, 4> yield{};
        if (merged.type == FourCC{"FLOR"}) {
            auto r = record::parse_flora(data, form_ctx);
            if (!r) {
                ++shared_.stats().parse_errors;
                return;
            }
            editor_id = r->editor_id;
            ingredient = r->ingredient;
            sound = r->harvest_sound;
            yield = r->seasonal_yield;
        } else {
            auto r = record::parse_tree(data, form_ctx);
            if (!r) {
                ++shared_.stats().parse_errors;
                return;
            }
            editor_id = r->editor_id;
            ingredient = r->ingredient;
            sound = r->harvest_sound;
            yield = r->seasonal_yield;
        }
        if (ingredient.value == 0) {
            return;
        }
        entry.editor_id = std::move(editor_id);
        entry.has_harvest = true;
        entry.ingredient = shared_.global(merged, ingredient, failed);
        entry.sound = shared_.global(merged, sound, failed);
        entry.seasonal_yield = pack_yield(yield);
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    work_entries_[merged.form.value] = std::move(entry);
}

void BaseCollector::on_recipe(const record::MergedRecord& merged, io::SpanReader& data,
                              const record::FormContext& form_ctx) {
    auto r = record::parse_constructible_object(data, form_ctx);
    if (!r) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    wfb::RecipeT out;
    out.id = merged.form.value;
    out.editor_id = r->editor_id;
    out.product = shared_.global(merged, r->created_object, failed);
    out.product_count = r->created_count;
    out.bench_keyword = shared_.global(merged, r->workbench_keyword, failed);
    for (const auto& it : r->items) {
        out.components.emplace_back(shared_.global(merged, it.item, failed), it.count);
    }
    out.conditions = shared_.global_conditions(merged, r->conditions, failed);
    if (failed) {
        ++shared_.stats().unresolved;
    }
    recipes_[out.id] = std::move(out);
}

void BaseCollector::on_magic_effect(const record::MergedRecord& merged, io::SpanReader& data,
                                    const record::FormContext& form_ctx) {
    auto r = record::parse_magic_effect(data, form_ctx);
    if (!r) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    wfb::MagicEffectT out;
    out.id = merged.form.value;
    out.editor_id = r->editor_id;
    out.name = r->name.text;
    out.description = r->description.text;
    out.flags = r->flags;
    out.base_cost = r->base_cost;
    out.magic_skill = r->magic_skill;
    out.actor_value = r->actor_value;
    out.archetype = r->archetype;
    out.casting_type = r->casting_type;
    out.delivery = r->delivery;
    out.keywords = shared_.global_all(merged, r->keywords.keywords, failed);
    if (failed) {
        ++shared_.stats().unresolved;
    }
    magic_effects_[out.id] = std::move(out);
}

void BaseCollector::on_enchantment(const record::MergedRecord& merged, io::SpanReader& data,
                                   const record::FormContext& form_ctx) {
    auto r = record::parse_enchantment(data, form_ctx);
    if (!r) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    wfb::EnchantmentT out;
    out.id = merged.form.value;
    out.editor_id = r->editor_id;
    out.name = r->name.text;
    out.cost = r->cost;
    out.charge = r->amount;
    out.effects = effect_items(shared_, merged, r->effects, failed);
    if (failed) {
        ++shared_.stats().unresolved;
    }
    enchantments_[out.id] = std::move(out);
}

void BaseCollector::on_game_setting(const record::MergedRecord& /*merged*/, io::SpanReader& data,
                                    const record::FormContext& form_ctx) {
    auto r = record::parse_game_setting(data, form_ctx);
    if (!r) {
        ++shared_.stats().parse_errors;
        return;
    }
    using Kind = record::GameSetting::Kind;
    wfb::GameSettingT out;
    out.editor_id = r->editor_id;
    switch (r->kind) {
    case Kind::floating:
        out.kind = wfb::GameSettingKind::floating;
        out.float_value = r->float_value;
        break;
    case Kind::integer:
        out.kind = wfb::GameSettingKind::integer;
        out.int_value = r->integer_value;
        break;
    case Kind::unsigned_int:
        out.kind = wfb::GameSettingKind::integer;
        out.int_value = static_cast<std::int32_t>(r->unsigned_value);
        break;
    case Kind::boolean:
        out.kind = wfb::GameSettingKind::boolean;
        out.int_value = r->boolean_value ? 1 : 0;
        break;
    case Kind::string:
        out.kind = wfb::GameSettingKind::text;
        out.text_value = r->text.text;
        break;
    case Kind::unknown:
        return;
    }
    if (out.editor_id.empty() || r->raw.empty()) {
        return;
    }
    gmsts_[skydot::formats::ascii_lower(out.editor_id)] = std::move(out);
}

void BaseCollector::on_impact_data_set(const record::MergedRecord& merged, io::SpanReader& data,
                                       const record::FormContext& form_ctx) {
    auto r = record::parse_impact_data_set(data, form_ctx);
    if (!r) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    wfb::ImpactDataSetT out;
    out.id = merged.form.value;
    out.editor_id = r->editor_id;
    for (const auto& entry : r->entries) {
        out.entries.emplace_back(shared_.global(merged, entry.material, failed),
                                 shared_.global(merged, entry.impact, failed));
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    impact_data_sets_[out.id] = std::move(out);
}

void BaseCollector::on_impact(const record::MergedRecord& merged, io::SpanReader& data,
                              const record::FormContext& form_ctx) {
    auto r = record::parse_impact(data, form_ctx);
    if (!r) {
        ++shared_.stats().parse_errors;
        return;
    }
    bool failed = false;
    wfb::ImpactT out;
    out.id = merged.form.value;
    out.editor_id = r->editor_id;
    out.model = r->model.empty() ? std::string{} : model_vpath(r->model.path);
    out.duration = r->duration;
    out.orientation = r->orientation;
    out.angle_threshold = r->angle_threshold;
    out.placement_radius = r->placement_radius;
    out.flags = r->flags;
    out.sound1 = shared_.global(merged, r->sound1, failed);
    out.sound2 = shared_.global(merged, r->sound2, failed);
    out.hazard = shared_.global(merged, r->hazard, failed);
    if (failed) {
        ++shared_.stats().unresolved;
    }
    impacts_[out.id] = std::move(out);
}

void BaseCollector::on_combat_style(const record::MergedRecord& merged, io::SpanReader& data,
                                    const record::FormContext& form_ctx) {
    auto r = record::parse_combat_style(data, form_ctx);
    if (!r) {
        ++shared_.stats().parse_errors;
        return;
    }
    wfb::CombatStyleT out;
    out.id = merged.form.value;
    out.editor_id = r->editor_id;
    out.offensive_mult = r->general[0];
    out.defensive_mult = r->general[1];
    out.group_offensive_mult = r->general[2];
    out.melee_score_mult = r->general[3];
    out.magic_score_mult = r->general[4];
    out.ranged_score_mult = r->general[5];
    out.shout_score_mult = r->general[6];
    out.unarmed_score_mult = r->general[7];
    out.staff_score_mult = r->general[8];
    out.avoid_threat_chance = r->general[9];
    out.attack_staggered_mult = r->melee[0];
    out.power_attack_staggered_mult = r->melee[1];
    out.power_attack_blocking_mult = r->melee[2];
    out.bash_mult = r->melee[3];
    out.bash_recoil_mult = r->melee[4];
    out.bash_attack_mult = r->melee[5];
    out.bash_power_attack_mult = r->melee[6];
    out.special_attack_mult = r->melee[7];
    out.circle_mult = r->close_range[0];
    out.fallback_mult = r->close_range[1];
    out.flank_distance = r->close_range[2];
    out.stalk_time = r->close_range[3];
    out.strafe_mult = r->long_range;
    out.hover_chance = r->flight[0];
    out.dive_bomb_chance = r->flight[1];
    out.ground_attack_chance = r->flight[2];
    out.hover_time = r->flight[3];
    out.ground_attack_time = r->flight[4];
    out.perch_attack_chance = r->flight[5];
    out.perch_attack_time = r->flight[6];
    out.flying_attack_chance = r->flight[7];
    out.flags = r->flags;
    combat_styles_[out.id] = std::move(out);
}

bool BaseCollector::is_item_type(io::FourCC type) noexcept {
    return type == FourCC{"ALCH"} || type == FourCC{"AMMO"} || type == FourCC{"ARMO"} ||
           type == FourCC{"BOOK"} || type == FourCC{"INGR"} || type == FourCC{"KEYM"} ||
           type == FourCC{"MISC"} || type == FourCC{"SLGM"} || type == FourCC{"SCRL"} ||
           type == FourCC{"WEAP"} || type == FourCC{"CONT"};
}

namespace {

/// The record header's Non-Playable flag (ARMO, WEAP, BOOK).
constexpr std::uint32_t k_record_non_playable = 0x4;

/// BOD2 slot bits (slot - 30) of the jewelry: amulet 35, ring 36, circlet 42.
constexpr std::uint32_t k_jewelry_slots = (1U << 5) | (1U << 6) | (1U << 12);

/// Armor type 2 (clothing) with only jewelry slots is jewelry.
wfb::ItemCategory armor_category(std::uint32_t armor_type, std::uint32_t slots) {
    if (armor_type != 2) {
        return wfb::ItemCategory::armor;
    }
    if (slots != 0 && (slots & ~k_jewelry_slots) == 0) {
        return wfb::ItemCategory::jewelry;
    }
    return wfb::ItemCategory::clothing;
}

} // namespace

void BaseCollector::collect_item(const record::MergedRecord& merged, io::SpanReader& data,
                                 const record::FormContext& form_ctx) {
    ItemEntry entry;
    entry.type = merged.type.value;
    entry.record_flags = merged.flags;
    entry.has_item = true;
    auto& item = entry.item;
    bool failed = false;
    const auto type = merged.type;
    const auto fail = [&] { ++shared_.stats().parse_errors; };
    if (type == FourCC{"MISC"}) {
        auto r = record::parse_misc_item(data, form_ctx);
        if (!r) { return fail(); }
        entry.editor_id = r->editor_id;
        entry.name = r->name.text;
        item.category = wfb::ItemCategory::misc;
        item.weight = r->weight;
        item.value = r->value;
    } else if (type == FourCC{"KEYM"}) {
        auto r = record::parse_key(data, form_ctx);
        if (!r) { return fail(); }
        entry.editor_id = r->editor_id;
        entry.name = r->name.text;
        item.category = wfb::ItemCategory::key;
        item.weight = r->weight;
        item.value = r->value;
    } else if (type == FourCC{"ALCH"}) {
        auto r = record::parse_ingestible(data, form_ctx);
        if (!r) { return fail(); }
        entry.editor_id = r->editor_id;
        entry.name = r->name.text;
        item.effects = effect_items(shared_, merged, r->effects, failed);
        item.food = r->has(record::Ingestible::Flag::food_item);
        item.category = item.food ? wfb::ItemCategory::food : wfb::ItemCategory::potion;
        item.weight = r->weight;
        item.value = r->value;
    } else if (type == FourCC{"INGR"}) {
        auto r = record::parse_ingredient(data, form_ctx);
        if (!r) { return fail(); }
        entry.editor_id = r->editor_id;
        entry.name = r->name.text;
        item.effects = effect_items(shared_, merged, r->effects, failed);
        item.category = wfb::ItemCategory::ingredient;
        item.weight = r->weight;
        item.value = r->value;
    } else if (type == FourCC{"BOOK"}) {
        auto r = record::parse_book(data, form_ctx);
        if (!r) { return fail(); }
        entry.editor_id = r->editor_id;
        entry.name = r->name.text;
        item.category = wfb::ItemCategory::book;
        item.weight = r->weight;
        item.value = r->value;
        item.playable = (merged.flags & k_record_non_playable) == 0;
    } else if (type == FourCC{"SLGM"}) {
        auto r = record::parse_soul_gem(data, form_ctx);
        if (!r) { return fail(); }
        entry.editor_id = r->editor_id;
        entry.name = r->name.text;
        item.category = wfb::ItemCategory::soul_gem;
        item.weight = r->weight;
        item.value = r->value;
    } else if (type == FourCC{"SCRL"}) {
        auto r = record::parse_scroll(data, form_ctx);
        if (!r) { return fail(); }
        entry.editor_id = r->editor_id;
        entry.name = r->name.text;
        item.effects = effect_items(shared_, merged, r->effects, failed);
        item.category = wfb::ItemCategory::scroll;
        item.weight = r->weight;
        item.value = r->value;
        item.equip_slot = shared_.global(merged, r->equip_slot, failed);
    } else if (type == FourCC{"AMMO"}) {
        auto r = record::parse_ammo(data, form_ctx);
        if (!r) { return fail(); }
        entry.editor_id = r->editor_id;
        entry.name = r->name.text;
        item.category = wfb::ItemCategory::ammo;
        item.weight = r->weight.value_or(0.0F);
        item.value = r->value;
        item.damage = r->damage;
        item.playable = !r->has(record::Ammo::Flag::non_playable);
    } else if (type == FourCC{"WEAP"}) {
        auto r = record::parse_weapon(data, form_ctx);
        if (!r) { return fail(); }
        entry.editor_id = r->editor_id;
        entry.name = r->name.text;
        item.category = wfb::ItemCategory::weapon;
        item.weight = r->weight;
        item.value = r->value;
        item.damage = static_cast<float>(r->damage);
        item.enchantment = shared_.global(merged, r->enchantment, failed);
        item.equip_slot = shared_.global(merged, r->equip_type, failed);
        // DNAM byte 0: the animation type.
        item.weapon_type = r->weapon_data.empty() ? std::uint8_t{0}
                                                  : static_cast<std::uint8_t>(r->weapon_data[0]);
        item.speed = r->speed;
        item.reach = r->reach;
        item.weapon_skill = static_cast<std::int32_t>(r->skill);
        item.crit_damage = r->crit_damage;
        item.crit_mult = r->crit_mult;
        item.stagger = r->stagger;
        item.impact_data_set = shared_.global(merged, r->impact_data_set, failed);
        item.equip_sound = shared_.global(merged, r->equip_sound, failed);
        item.unequip_sound = shared_.global(merged, r->unequip_sound, failed);
        entry.first_person_stat = shared_.global(merged, r->first_person_model, failed);
        item.playable = (merged.flags & k_record_non_playable) == 0;
    } else if (type == FourCC{"ARMO"}) {
        auto r = record::parse_armor(data, form_ctx);
        if (!r) { return fail(); }
        entry.editor_id = r->editor_id;
        entry.name = r->name.text;
        item.weight = r->weight;
        item.value = static_cast<std::int32_t>(r->value);
        item.armor_rating = static_cast<float>(r->armor_rating) / 100.0F;
        item.enchantment = shared_.global(merged, r->enchantment, failed);
        item.armor_type = static_cast<std::uint8_t>(r->body.armor_type);
        item.biped_slots = r->body.slots;
        item.category = armor_category(r->body.armor_type, r->body.slots);
        item.equip_slot = shared_.global(merged, r->equip_slot, failed);
        item.playable = (merged.flags & k_record_non_playable) == 0;
    } else if (type == FourCC{"CONT"}) {
        auto r = record::parse_container(data, form_ctx);
        if (!r) { return fail(); }
        entry.has_item = false;
        entry.editor_id = r->editor_id;
        entry.name = r->name.text;
        entry.flags = r->flags;
        for (const auto& it : r->items) {
            entry.items.emplace_back(shared_.global(merged, it.item, failed), it.count);
        }
    }
    if (failed) {
        ++shared_.stats().unresolved;
    }
    item_entries_[merged.form.value] = std::move(entry);
}

void BaseCollector::finish(wfb::WorldT& world) {
    auto& stats = shared_.stats();
    for (auto& [id, entry] : item_entries_) {
        auto found = bases_.find(id);
        if (found == bases_.end()) {
            // An item without a model or scripts: a base all the same, so its
            // references and the inventories that hold it resolve.
            if (!entry.has_item) {
                continue;
            }
            wfb::BaseT made;
            made.id = id;
            made.type = entry.type;
            made.editor_id = entry.editor_id;
            made.record_flags = static_cast<wfb::RecordFlags>(entry.record_flags);
            found = bases_.emplace(id, std::move(made)).first;
        }
        auto& base = found->second;
        if (base.name.empty()) {
            base.name = entry.name;
        }
        if (entry.has_item) {
            if (entry.first_person_stat != 0) {
                if (const auto stat = bases_.find(entry.first_person_stat); stat != bases_.end()) {
                    entry.item.first_person_model = stat->second.model;
                }
            }
            base.item = std::make_unique<wfb::ItemDataT>(entry.item);
            ++stats.items;
        } else {
            base.flags = entry.flags;
            base.items = std::move(entry.items);
            ++stats.containers;
            stats.container_items += base.items.size();
        }
    }
    item_entries_.clear();
    for (auto& [id, entry] : work_entries_) {
        auto found = bases_.find(id);
        if (found == bases_.end()) {
            if (!entry.has_harvest) {
                continue;
            }
            wfb::BaseT made;
            made.id = id;
            made.type = entry.type;
            made.editor_id = entry.editor_id;
            made.record_flags = static_cast<wfb::RecordFlags>(entry.record_flags);
            found = bases_.emplace(id, std::move(made)).first;
        }
        auto& base = found->second;
        base.bench_type = entry.bench_type;
        base.bench_skill = entry.bench_skill;
        if (entry.has_harvest) {
            auto harvest = std::make_unique<wfb::HarvestT>();
            harvest->ingredient = entry.ingredient;
            harvest->sound = entry.sound;
            harvest->seasonal_yield = entry.seasonal_yield;
            base.harvest = std::move(harvest);
            ++stats.harvestables;
        } else {
            ++stats.benches;
        }
    }
    work_entries_.clear();
    stats.gmsts += gmsts_.size();
    stats.impact_data_sets += impact_data_sets_.size();
    stats.impacts += impacts_.size();
    stats.combat_styles += combat_styles_.size();
    stats.recipes += recipes_.size();
    stats.magic_effects += magic_effects_.size();
    stats.enchantments += enchantments_.size();
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
    move_into(world.recipes, recipes_);
    move_into(world.impact_data_sets, impact_data_sets_);
    move_into(world.impacts, impacts_);
    move_into(world.combat_styles, combat_styles_);
    for (auto& [name, setting] : gmsts_) {
        world.gmsts.push_back(std::make_unique<wfb::GameSettingT>(std::move(setting)));
    }
    move_into(world.magic_effects, magic_effects_);
    move_into(world.enchantments, enchantments_);
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
