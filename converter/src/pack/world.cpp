// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/world.hpp"

#include "bethconv/archive/vpath.hpp"
#include "bethconv/io/byte_view.hpp"
#include "bethconv/io/mapped_file.hpp"
#include "bethconv/io/span_stream.hpp"
#include "bethconv/record/field_walk.hpp"
#include "bethconv/record/forms.hpp"
#include "bethconv/record/forms_actor.hpp"
#include "bethconv/record/forms_game.hpp"
#include "bethconv/record/forms_object.hpp"
#include "bethconv/record/forms_world.hpp"
#include "bethconv/record/types.hpp"

#include "world/context.hpp"
#include "world/fb_write.hpp"
#include "world/places.hpp"

#include "bethconv/pack/world_generated.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

namespace bethconv::pack {
namespace {

using detail::model_vpath;
using detail::texture_vpath;
using detail::write_scripts;
using io::FourCC;
using record::FormId;

struct TextureSetEntry {
    std::string diffuse;
    std::string normal;
};

struct LandTextureEntry {
    std::uint32_t id{};
    std::string editor_id;
    std::uint32_t texture_set{};
    std::uint8_t specular{};
    std::vector<std::uint32_t> grasses;
};

/// ADDN; see world.fbs `AddonNode`.
struct AddonEntry {
    std::uint32_t id{};
    std::string editor_id;
    std::int32_t index{};
    std::string model;
};

/// GRAS; see world.fbs `Grass`.
struct GrassEntry {
    std::uint32_t id{};
    std::string editor_id;
    std::string model;
    std::uint8_t density{};
    std::uint8_t min_slope{};
    std::uint8_t max_slope{};
    std::uint16_t units_from_water{};
    std::uint32_t water_type{};
    float position_range{};
    float height_range{};
    float color_range{};
    float wave_period{};
    std::uint8_t flags{};
};

struct BaseEntry {
    std::uint32_t id{};
    std::uint32_t type{};
    std::string editor_id;
    std::string model;
    std::optional<WorldLight> light;
    std::uint32_t flags{};
    std::vector<record::Script> scripts;
    std::uint32_t record_flags{};
    std::uint32_t directional_material{};
    float directional_max_angle{};
};

/// MATO; see world.fbs `MaterialObject`.
struct MaterialObjectEntry {
    std::uint32_t id{};
    std::string editor_id;
    std::string model;
    std::array<float, 11> data{}; ///< DATA's first 44 bytes
    std::uint32_t flags{};         ///< DATA's last word; bit 0 single pass
};

/// Types whose VMAD is followed by fragment data; their scripts only run
/// through systems that do not exist yet.
bool has_fragments(FourCC type) {
    return type == FourCC{"QUST"} || type == FourCC{"INFO"} || type == FourCC{"PACK"} ||
           type == FourCC{"SCEN"} || type == FourCC{"PERK"};
}

class WorldSink final : public record::MergedRecordSink {
public:
    explicit WorldSink(const record::LoadOrder& order) : shared_(order), places_(shared_) {}

    void on_record(const record::MergedRecord& merged, const record::RecordContext& ctx,
                   io::SpanReader& data, const record::FormContext& form_ctx) override {
        if (merged.deleted) {
            return;
        }
        if (merged.type == FourCC{"CELL"} || merged.type == FourCC{"REFR"} ||
            merged.type == FourCC{"ACHR"} || merged.type == FourCC{"LAND"} ||
            merged.type == FourCC{"NAVM"} || merged.type == FourCC{"LGTM"}) {
            places_.collect(merged, ctx, data, form_ctx);
        } else if (merged.type == FourCC{"LIGH"}) {
            on_light(merged, data, form_ctx);
        } else if (merged.type == FourCC{"WRLD"}) {
            on_worldspace(merged, data, form_ctx);
        } else if (merged.type == FourCC{"LTEX"}) {
            on_land_texture(merged, data, form_ctx);
        } else if (merged.type == FourCC{"TXST"}) {
            on_texture_set(merged, data, form_ctx);
        } else if (merged.type == FourCC{"WATR"}) {
            on_water(merged, data);
        } else if (merged.type == FourCC{"CLMT"}) {
            on_climate(merged, data, form_ctx);
        } else if (merged.type == FourCC{"WTHR"}) {
            on_weather(merged, data, form_ctx);
        } else if (merged.type == FourCC{"IMGS"}) {
            on_image_space(merged, data, form_ctx);
        } else if (merged.type == FourCC{"MATO"}) {
            on_material_object(merged, data);
        } else if (merged.type == FourCC{"GRAS"}) {
            on_grass(merged, data);
        } else if (merged.type == FourCC{"ADDN"}) {
            on_addon_node(merged, data);
        } else if (merged.type == FourCC{"QUST"}) {
            on_quest(merged, data, form_ctx);
        } else if (merged.type == FourCC{"GLOB"}) {
            on_global(merged, data, form_ctx);
        } else if (merged.type == FourCC{"SPGD"}) {
            on_precipitation(merged, data, form_ctx);
        } else if (merged.type == FourCC{"REGN"}) {
            on_region(merged, data, form_ctx);
        } else if (merged.type == FourCC{"RACE"}) {
            on_race(merged, data, form_ctx);
        } else if (merged.type == FourCC{"ARMA"}) {
            on_armor_addon(merged, data, form_ctx);
        } else if (merged.type == FourCC{"OTFT"}) {
            on_outfit(merged, data, form_ctx);
        } else if (merged.type == FourCC{"LVLI"}) {
            on_leveled_item(merged, data, form_ctx);
        } else if (merged.type == FourCC{"PACK"}) {
            on_package(merged, data, form_ctx);
        } else if (merged.type == FourCC{"FLST"}) {
            on_form_list(merged, data, form_ctx);
        } else if (merged.type != FourCC{"INFO"}) {
            // These are bases too (placed armor, scripted NPCs); read twice.
            io::SpanReader copy = data;
            if (merged.type == FourCC{"NPC_"}) {
                on_npc(merged, copy, form_ctx);
            } else if (merged.type == FourCC{"ARMO"}) {
                on_armor(merged, copy, form_ctx);
            } else if (merged.type == FourCC{"LVLN"}) {
                on_leveled_npc(merged, copy, form_ctx);
            }
            on_other(merged, data);
        }
    }

    [[nodiscard]] WorldStats& stats() noexcept { return shared_.stats(); }
    [[nodiscard]] detail::PlaceCollector& places() noexcept { return places_; }
    [[nodiscard]] std::map<std::uint32_t, WorldNpc>& npcs() noexcept { return npcs_; }
    [[nodiscard]] std::map<std::uint32_t, WorldRace>& races() noexcept { return races_; }
    [[nodiscard]] std::map<std::uint32_t, WorldArmor>& armors() noexcept { return armors_; }
    [[nodiscard]] std::map<std::uint32_t, WorldArmorAddon>& armor_addons() noexcept {
        return armor_addons_;
    }
    [[nodiscard]] std::map<std::uint32_t, WorldOutfit>& outfits() noexcept { return outfits_; }
    [[nodiscard]] std::map<std::uint32_t, WorldLeveledList>& leveled_lists() noexcept {
        return leveled_lists_;
    }
    [[nodiscard]] std::map<std::uint32_t, WorldPackage>& packages() noexcept { return packages_; }
    [[nodiscard]] const std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>& form_lists()
        const noexcept {
        return form_lists_;
    }
    [[nodiscard]] std::map<std::uint32_t, BaseEntry>& bases() noexcept { return bases_; }
    [[nodiscard]] std::map<std::uint32_t, Worldspace>& worlds() noexcept { return worlds_; }
    [[nodiscard]] std::map<std::uint32_t, LandTextureEntry>& land_textures() noexcept {
        return land_textures_;
    }
    [[nodiscard]] std::map<std::uint32_t, WorldWater>& waters() noexcept { return waters_; }
    [[nodiscard]] std::map<std::uint32_t, WorldClimate>& climates() noexcept { return climates_; }
    [[nodiscard]] std::map<std::uint32_t, WorldWeather>& weathers() noexcept { return weathers_; }
    [[nodiscard]] std::map<std::uint32_t, WorldImageSpace>& image_spaces() noexcept {
        return image_spaces_;
    }
    [[nodiscard]] const std::map<std::uint32_t, GrassEntry>& grasses() const noexcept { return grasses_; }
    [[nodiscard]] const std::map<std::uint32_t, AddonEntry>& addons() const noexcept { return addons_; }
    [[nodiscard]] const std::map<std::uint32_t, MaterialObjectEntry>& material_objects() const noexcept {
        return material_objects_;
    }
    [[nodiscard]] std::map<std::uint32_t, WorldPrecipitation>& precipitations() noexcept {
        return precipitations_;
    }
    [[nodiscard]] std::map<std::uint32_t, WorldRegion>& regions() noexcept { return regions_; }
    [[nodiscard]] std::unordered_map<std::uint32_t, TextureSetEntry>& texture_sets() noexcept {
        return texture_sets_;
    }
    [[nodiscard]] std::map<std::uint32_t, WorldQuest>& quests() noexcept { return quests_; }
    [[nodiscard]] std::map<std::uint32_t, WorldGlobal>& globals() noexcept { return globals_; }

private:
    void on_light(const record::MergedRecord& merged, io::SpanReader& data,
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
                    .flags = light->light_flags,
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
            .record_flags = merged.flags,
        };
        bool failed = false;
        bases_[merged.form.value].scripts = shared_.global_scripts(merged, std::move(light->scripts), failed);
        if (failed) {
            ++shared_.stats().unresolved;
        }
    }

    void on_quest(const record::MergedRecord& merged, io::SpanReader& data,
                  const record::FormContext& form_ctx) {
        auto q = record::parse_quest(data, form_ctx);
        if (!q) {
            ++shared_.stats().parse_errors;
            return;
        }
        bool failed = false;
        WorldQuest out{
            .id = merged.form.value,
            .editor_id = q->editor_id,
            .name = q->name.text,
            .flags = q->flags,
            .priority = q->priority,
            .type = q->type,
            .event = q->event.value,
            .scripts = shared_.global_scripts(merged, std::move(q->scripts), failed),
            .fragment_script = q->fragments.script,
            .fragments = {},
            .stages = {},
            .objectives = {},
            .aliases = {},
        };
        for (const auto& f : q->fragments.fragments) {
            out.fragments.push_back(WorldQuestFragment{
                .stage = f.stage, .log_entry = f.log_entry, .function = f.function});
            if (out.fragment_script.empty()) {
                out.fragment_script = f.script;
            }
        }
        std::ranges::stable_sort(out.fragments, [](const auto& a, const auto& b) {
            return std::pair{a.stage, a.log_entry} < std::pair{b.stage, b.log_entry};
        });
        for (const auto& stage : q->stages) {
            auto& s = out.stages.emplace_back();
            s.index = stage.index;
            s.flags = stage.flags;
            for (const auto& entry : stage.log) {
                s.log.push_back(WorldQuestLogEntry{
                    .flags = entry.flags,
                    .text = entry.text.text,
                    .conditions = static_cast<std::uint16_t>(entry.conditions.raw.size())});
            }
        }
        std::ranges::stable_sort(out.stages, {}, &WorldQuestStage::index);
        for (const auto& objective : q->objectives) {
            auto& o = out.objectives.emplace_back();
            o.index = objective.index;
            o.flags = objective.flags;
            o.text = objective.text.text;
            for (const auto& target : objective.targets) {
                o.targets.push_back(target.alias);
            }
        }
        for (const auto& alias : q->aliases) {
            WorldQuestAlias a{
                .id = alias.id,
                .name = alias.name,
                .location = alias.location,
                .flags = alias.flags,
                .forced = shared_.global(merged, alias.location ? alias.specific_location
                                                        : alias.forced_ref,
                                 failed),
                .unique_actor = shared_.global(merged, alias.unique_actor, failed),
                .external_quest = shared_.global(merged, alias.external_quest, failed),
                .external_alias = alias.external_alias,
                .created_object = shared_.global(merged, alias.created_object, failed),
                .create_at = alias.create_at,
                .conditions = static_cast<std::uint16_t>(alias.conditions.raw.size()),
                .display_name = shared_.global(merged, alias.display_name, failed),
                .scripts = {},
            };
            for (auto& attached : q->fragments.aliases) {
                if (attached.alias.alias >= 0 &&
                    static_cast<std::uint32_t>(attached.alias.alias) == alias.id) {
                    record::ScriptData data_for_alias;
                    data_for_alias.scripts = std::move(attached.scripts);
                    a.scripts = shared_.global_scripts(merged, std::move(data_for_alias), failed);
                }
            }
            out.aliases.push_back(std::move(a));
        }
        if (failed) {
            ++shared_.stats().unresolved;
        }
        quests_[out.id] = std::move(out);
    }

    void on_global(const record::MergedRecord& merged, io::SpanReader& data,
                   const record::FormContext& form_ctx) {
        auto g = record::parse_global(data, form_ctx);
        if (!g) {
            ++shared_.stats().parse_errors;
            return;
        }
        globals_[merged.form.value] = WorldGlobal{
            .id = merged.form.value, .editor_id = g->editor_id, .kind = g->kind, .value = g->value};
    }

    // ---- what actors are built from -------------------------------------

    /// The precomputed FaceGen head: named by the plugin owning the form and
    /// the form's id within it.
    std::string face_model(const record::MergedRecord& merged) const {
        const auto& entries = shared_.order().entries();
        if (merged.owner >= entries.size()) {
            return {};
        }
        const auto& owner = entries[merged.owner];
        std::array<char, 16> id{};
        std::snprintf(id.data(), id.size(), "%08x", merged.form.value & owner.object_mask());
        std::string plugin = owner.name;
        std::ranges::transform(plugin, plugin.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string hex(id.data());
        return "meshes/actors/character/facegendata/facegeom/" + plugin + "/" + hex + ".nif";
    }

    void on_npc(const record::MergedRecord& merged, io::SpanReader& data,
                const record::FormContext& form_ctx) {
        auto npc = record::parse_npc(data, form_ctx);
        if (!npc) {
            ++shared_.stats().parse_errors;
            return;
        }
        bool failed = false;
        WorldNpc out;
        out.id = merged.form.value;
        out.editor_id = npc->editor_id;
        out.name = npc->name.text;
        out.flags = npc->flags;
        out.level = npc->level;
        out.race = shared_.global(merged, npc->race, failed);
        out.template_form = shared_.global(merged, npc->npc_template, failed);
        out.template_flags = npc->template_flags;
        out.skin = shared_.global(merged, npc->worn_armor, failed);
        out.default_outfit = shared_.global(merged, npc->default_outfit, failed);
        out.sleeping_outfit = shared_.global(merged, npc->sleeping_outfit, failed);
        out.height = npc->height;
        out.weight = npc->weight;
        out.head_parts = shared_.global_all(merged, npc->head_parts, failed);
        out.packages = shared_.global_all(merged, npc->packages, failed);
        out.default_package_list = shared_.global(merged, npc->default_package_list, failed);
        for (const auto& f : npc->factions) {
            out.factions.emplace_back(shared_.global(merged, f.faction, failed), f.rank);
        }
        for (const auto& item : npc->items) {
            out.items.emplace_back(shared_.global(merged, item.item, failed), item.count);
        }
        out.face_model = face_model(merged);
        out.skin_tone = {npc->skin_red, npc->skin_green, npc->skin_blue};
        if (failed) {
            ++shared_.stats().unresolved;
        }
        npcs_[out.id] = std::move(out);
    }

    void on_race(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx) {
        auto race = record::parse_race(data, form_ctx);
        if (!race) {
            ++shared_.stats().parse_errors;
            return;
        }
        bool failed = false;
        WorldRace out;
        out.id = merged.form.value;
        out.editor_id = race->editor_id;
        for (std::size_t sex = 0; sex < 2; ++sex) {
            const auto& s = race->sexes[sex];
            out.skeletons[sex] = s.skeleton.empty() ? std::string{} : model_vpath(s.skeleton);
            out.behaviours[sex] = s.behaviour.empty() ? std::string{} : model_vpath(s.behaviour);
            for (const auto& part : s.body_parts) {
                out.body_parts.push_back(WorldRace::BodyPart{
                    .female = sex == 1,
                    .index = part.index,
                    .model = part.model.empty() ? std::string{} : model_vpath(part.model)});
            }
            out.head_parts[sex] = shared_.global_all(merged, s.head_parts, failed);
        }
        out.skin = shared_.global(merged, race->skin, failed);
        out.heights = race->height;
        out.weights = race->weight;
        out.flags = race->flags;
        out.armor_race = shared_.global(merged, race->armor_race, failed);
        if (failed) {
            ++shared_.stats().unresolved;
        }
        races_[out.id] = std::move(out);
    }

    void on_armor(const record::MergedRecord& merged, io::SpanReader& data,
                  const record::FormContext& form_ctx) {
        auto armor = record::parse_armor(data, form_ctx);
        if (!armor) {
            ++shared_.stats().parse_errors;
            return;
        }
        bool failed = false;
        WorldArmor out{.id = merged.form.value,
                       .editor_id = armor->editor_id,
                       .slots = armor->body.slots,
                       .race = shared_.global(merged, armor->race, failed),
                       .addons = shared_.global_all(merged, armor->addons, failed)};
        if (failed) {
            ++shared_.stats().unresolved;
        }
        armors_[out.id] = std::move(out);
    }

    void on_armor_addon(const record::MergedRecord& merged, io::SpanReader& data,
                        const record::FormContext& form_ctx) {
        auto addon = record::parse_armor_addon(data, form_ctx);
        if (!addon) {
            ++shared_.stats().parse_errors;
            return;
        }
        bool failed = false;
        WorldArmorAddon out;
        out.id = merged.form.value;
        out.editor_id = addon->editor_id;
        out.slots = addon->body.slots;
        out.race = shared_.global(merged, addon->race, failed);
        out.additional_races = shared_.global_all(merged, addon->additional_races, failed);
        out.models = {addon->male_model.empty() ? std::string{} : model_vpath(addon->male_model.path),
                      addon->female_model.empty() ? std::string{} : model_vpath(addon->female_model.path)};
        out.priorities = {addon->male_priority, addon->female_priority};
        out.weight_sliders = {addon->male_weight_slider, addon->female_weight_slider};
        if (failed) {
            ++shared_.stats().unresolved;
        }
        armor_addons_[out.id] = std::move(out);
    }

    void on_outfit(const record::MergedRecord& merged, io::SpanReader& data,
                   const record::FormContext& form_ctx) {
        auto outfit = record::parse_outfit(data, form_ctx);
        if (!outfit) {
            ++shared_.stats().parse_errors;
            return;
        }
        bool failed = false;
        outfits_[merged.form.value] =
            WorldOutfit{.id = merged.form.value, .items = shared_.global_all(merged, outfit->items, failed)};
        if (failed) {
            ++shared_.stats().unresolved;
        }
    }

    template <typename Entries>
    void add_leveled(const record::MergedRecord& merged, std::uint8_t flags, std::uint8_t chance_none,
                     const Entries& entries) {
        bool failed = false;
        WorldLeveledList out{.id = merged.form.value,
                             .type = merged.type.value,
                             .flags = flags,
                             .chance_none = chance_none,
                             .entries = {}};
        for (const auto& e : entries) {
            out.entries.push_back(
                {.level = e.level, .count = e.count, .form = shared_.global(merged, e.reference, failed)});
        }
        if (failed) {
            ++shared_.stats().unresolved;
        }
        leveled_lists_[out.id] = std::move(out);
    }

    /// FLST: kept to expand NPCs' default package lists.
    void on_form_list(const record::MergedRecord& merged, io::SpanReader& data,
                      const record::FormContext& form_ctx) {
        auto list = record::parse_form_list(data, form_ctx);
        if (!list) {
            ++shared_.stats().parse_errors;
            return;
        }
        bool failed = false;
        form_lists_[merged.form.value] = shared_.global_all(merged, list->forms, failed);
        if (failed) {
            ++shared_.stats().unresolved;
        }
    }

    /// A condition with its FormID parameters made global.
    record::Condition global_condition(const record::MergedRecord& merged, record::Condition c,
                                       bool& failed) {
        c.value_global = FormId{shared_.global(merged, c.value_global, failed)};
        if (c.run_on == record::Condition::k_run_on_reference) {
            c.reference = FormId{shared_.global(merged, c.reference, failed)};
        }
        if (record::condition_param_is_form(c, 1)) {
            c.param1 = shared_.global(merged, FormId{c.param1}, failed);
        }
        if (record::condition_param_is_form(c, 2)) {
            c.param2 = shared_.global(merged, FormId{c.param2}, failed);
        }
        return c;
    }

    std::vector<record::Condition> global_conditions(const record::MergedRecord& merged,
                                                     std::vector<record::Condition> list,
                                                     bool& failed) {
        for (auto& c : list) {
            c = global_condition(merged, std::move(c), failed);
        }
        return list;
    }

    void on_package(const record::MergedRecord& merged, io::SpanReader& data,
                    const record::FormContext& form_ctx) {
        auto pack = record::parse_package(data, form_ctx);
        if (!pack) {
            ++shared_.stats().parse_errors;
            return;
        }
        bool failed = false;
        WorldPackage out{
            .id = merged.form.value,
            .editor_id = pack->editor_id,
            .type = pack->type,
            .flags = pack->flags,
            .interrupt_override = pack->interrupt_override,
            .speed = pack->preferred_speed,
            .interrupt_flags = pack->interrupt_flags,
            .schedule = pack->schedule,
            .conditions = global_conditions(merged, std::move(pack->conditions), failed),
            .template_package = shared_.global(merged, pack->template_package, failed),
            .idle_flags = pack->idle_flags,
            .idle_timer = pack->idle_timer,
            .idles = shared_.global_all(merged, pack->idles, failed),
            .owner_quest = shared_.global(merged, pack->owner_quest, failed),
            .combat_style = shared_.global(merged, pack->combat_style, failed),
            .on_begin_idle = shared_.global(merged, pack->on_begin.idle, failed),
            .on_end_idle = shared_.global(merged, pack->on_end.idle, failed),
            .on_change_idle = shared_.global(merged, pack->on_change.idle, failed),
        };
        for (const auto& in : pack->inputs) {
            WorldPackage::Input w{.key = in.key, .type = in.type};
            // CNAM: one byte for Bool, a word otherwise; Float and ObjectList
            // (a radius) hold floats, Int an integer.
            io::SpanReader v{in.value, "PACK CNAM"};
            if (in.value.size() == 1) {
                w.number = static_cast<float>(v.get<std::uint8_t>().value_or(0));
            } else if (in.value.size() == 4) {
                w.number = in.type == "Int" ? static_cast<float>(v.get<std::int32_t>().value_or(0))
                                            : v.get<float>().value_or(0.0F);
            }
            if (in.location) {
                w.location = *in.location;
                const auto t = w.location.type;
                if (t == 0 || t == 1 || t == 4 || t == 6) {
                    w.location.value = shared_.global(merged, FormId{w.location.value}, failed);
                }
            }
            if (in.target) {
                w.target = *in.target;
                const auto t = w.target.type;
                if (t == 0 || t == 1 || t == 3) {
                    w.target.value = shared_.global(merged, FormId{w.target.value}, failed);
                }
            }
            for (const auto& named : pack->public_inputs) {
                if (named.key == in.key) {
                    w.name = named.name;
                }
            }
            out.inputs.push_back(std::move(w));
        }
        for (auto& b : pack->branches) {
            WorldPackage::Branch w{
                .type = b.type,
                .conditions = global_conditions(merged, std::move(b.conditions), failed),
                .children = b.branch_count,
                .flags = b.root_flags,
                .procedure = b.procedure,
                .success_completes = b.success_completes,
                .inputs = b.input_keys,
            };
            if (!b.flag_overrides.empty()) {
                const auto& o = b.flag_overrides.front();
                w.set_flags = o.set_flags;
                w.clear_flags = o.clear_flags;
                w.speed = static_cast<std::int8_t>(o.speed);
            }
            out.branches.push_back(std::move(w));
        }
        if (failed) {
            ++shared_.stats().unresolved;
        }
        packages_[out.id] = std::move(out);
    }

    void on_leveled_item(const record::MergedRecord& merged, io::SpanReader& data,
                         const record::FormContext& form_ctx) {
        auto list = record::parse_leveled_item(data, form_ctx);
        if (!list) {
            ++shared_.stats().parse_errors;
            return;
        }
        add_leveled(merged, list->flags, list->chance_none, list->entries);
    }

    void on_leveled_npc(const record::MergedRecord& merged, io::SpanReader& data,
                        const record::FormContext& form_ctx) {
        auto list = record::parse_leveled_npc(data, form_ctx);
        if (!list) {
            ++shared_.stats().parse_errors;
            return;
        }
        add_leveled(merged, list->flags, list->chance_none, list->entries);
    }

    /// Any other type: keep it as a base if it has a model or scripts. ARMO's
    /// MODL is an armature FormID, not a path; its world model is MOD2 (male)
    /// or MOD4 (female). Source: UESP, ARMO record.
    void on_other(const record::MergedRecord& merged, io::SpanReader& data) {
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
            .scripts = shared_.global_scripts(merged, std::move(scripts), failed),
            .record_flags = merged.flags,
            .directional_material = material != 0 ? shared_.global(merged, record::FormId{material}, failed) : 0,
            .directional_max_angle = max_angle,
        };
        if (failed) {
            ++shared_.stats().unresolved;
        }
    }

    void on_material_object(const record::MergedRecord& merged, io::SpanReader& data) {
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

    void on_worldspace(const record::MergedRecord& merged, io::SpanReader& data,
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
            .parent_flags = w->parent_flags,
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

    void on_land_texture(const record::MergedRecord& merged, io::SpanReader& data,
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

    void on_addon_node(const record::MergedRecord& merged, io::SpanReader& data) {
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

    void on_grass(const record::MergedRecord& merged, io::SpanReader& data) {
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
                    out.flags = body.get<std::uint8_t>().value_or(0);
                }
            });
        if (!walked || out.model.empty()) {
            ++shared_.stats().parse_errors;
            return;
        }
        grasses_[out.id] = std::move(out);
    }

    void on_texture_set(const record::MergedRecord& merged, io::SpanReader& data,
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

    /// WATR is not one of the record layer's types; only what rendering needs
    /// is read here. DNAM offsets: UESP's field order, checked against
    /// Skyrim.esm (228 bytes, 232 in some SE records).
    void on_water(const record::MergedRecord& merged, io::SpanReader& data) {
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

    void on_climate(const record::MergedRecord& merged, io::SpanReader& data,
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
    void on_weather(const record::MergedRecord& merged, io::SpanReader& data,
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
            out.classification = d[11];
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
    void on_image_space(const record::MergedRecord& merged, io::SpanReader& data,
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

    /// Cloud layers: textures (00TX..), speeds (QNAM, RNAM: one byte each,
    /// 127 still), colours by time (PNAM, RGBA), alphas by time (JNAM) and
    /// NAM1, whose set bits disable a layer. Layer i is drawn on the i-th
    /// shape of meshes/sky/clouds.nif.
    static void read_clouds(const record::Weather& weather, WorldWeather& out) {
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

    void on_precipitation(const record::MergedRecord& merged, io::SpanReader& data,
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
    void on_region(const record::MergedRecord& merged, io::SpanReader& data,
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

    detail::CollectContext shared_;
    detail::PlaceCollector places_;
    std::map<std::uint32_t, WorldWater> waters_;
    std::map<std::uint32_t, WorldClimate> climates_;
    std::map<std::uint32_t, WorldWeather> weathers_;
    std::map<std::uint32_t, WorldImageSpace> image_spaces_;
    std::map<std::uint32_t, MaterialObjectEntry> material_objects_;
    std::map<std::uint32_t, GrassEntry> grasses_;
    std::map<std::uint32_t, AddonEntry> addons_;
    std::map<std::uint32_t, WorldPrecipitation> precipitations_;
    std::map<std::uint32_t, WorldRegion> regions_;
    std::map<std::uint32_t, Worldspace> worlds_;
    std::map<std::uint32_t, LandTextureEntry> land_textures_;
    std::unordered_map<std::uint32_t, TextureSetEntry> texture_sets_;
    std::map<std::uint32_t, BaseEntry> bases_;
    std::map<std::uint32_t, WorldQuest> quests_;
    std::map<std::uint32_t, WorldGlobal> globals_;
    std::map<std::uint32_t, WorldNpc> npcs_;
    std::map<std::uint32_t, WorldRace> races_;
    std::map<std::uint32_t, WorldArmor> armors_;
    std::map<std::uint32_t, WorldArmorAddon> armor_addons_;
    std::map<std::uint32_t, WorldOutfit> outfits_;
    std::map<std::uint32_t, WorldLeveledList> leveled_lists_;
    std::map<std::uint32_t, WorldPackage> packages_;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> form_lists_;
};

} // namespace

io::ParseResult<WorldStats> write_world(const record::MergedWorld& world,
                                        const record::LoadOrder& order,
                                        const std::filesystem::path& out) {
    WorldSink sink(order);
    world.for_each_record(sink);
    auto& stats = sink.stats();

    flatbuffers::FlatBufferBuilder builder(1u << 20);

    const auto cells = sink.places().write_cells(builder);

    std::vector<flatbuffers::Offset<wfb::Base>> bases;
    bases.reserve(sink.bases().size());
    for (const auto& [id, base] : sink.bases()) {
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

    std::vector<flatbuffers::Offset<wfb::Worldspace>> worlds;
    for (const auto& [id, w] : sink.worlds()) {
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

    std::vector<flatbuffers::Offset<wfb::LandTexture>> land_textures;
    for (const auto& [id, ltex] : sink.land_textures()) {
        TextureSetEntry paths;
        if (const auto t = sink.texture_sets().find(ltex.texture_set);
            t != sink.texture_sets().end()) {
            paths = t->second;
        }
        land_textures.push_back(wfb::CreateLandTexture(
            builder, ltex.id, builder.CreateString(ltex.editor_id),
            builder.CreateString(paths.diffuse), builder.CreateString(paths.normal),
            ltex.specular, builder.CreateVector(ltex.grasses)));
        ++stats.land_textures;
    }

    std::vector<flatbuffers::Offset<wfb::Water>> waters;
    for (const auto& [id, w] : sink.waters()) {
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

    std::vector<flatbuffers::Offset<wfb::Climate>> climates;
    for (const auto& [id, c] : sink.climates()) {
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
    std::vector<flatbuffers::Offset<wfb::Weather>> weathers;
    for (const auto& [id, w] : sink.weathers()) {
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
    std::vector<flatbuffers::Offset<wfb::ImageSpace>> image_spaces;
    for (const auto& [id, i] : sink.image_spaces()) {
        image_spaces.push_back(wfb::CreateImageSpace(
            builder, i.id, builder.CreateString(i.editor_id), builder.CreateVector(i.hdr),
            builder.CreateVector(i.cinematic), builder.CreateVector(i.tint)));
        ++stats.image_spaces;
    }
    std::vector<flatbuffers::Offset<wfb::MaterialObject>> material_objects;
    for (const auto& [id, m] : sink.material_objects()) {
        const auto& d = m.data;
        const std::array<float, 3> projection{d[4], d[5], d[6]};
        const std::array<float, 3> colour{d[8], d[9], d[10]};
        material_objects.push_back(wfb::CreateMaterialObject(
            builder, m.id, builder.CreateString(m.editor_id), builder.CreateString(m.model), d[0], d[1],
            d[2], d[3], builder.CreateVector(projection.data(), projection.size()), d[7],
            builder.CreateVector(colour.data(), colour.size()), (m.flags & 1U) != 0));
    }
    std::vector<flatbuffers::Offset<wfb::Precipitation>> precipitations;
    for (const auto& [id, p] : sink.precipitations()) {
        precipitations.push_back(wfb::CreatePrecipitation(
            builder, p.id, builder.CreateString(p.editor_id), builder.CreateString(p.texture),
            p.gravity_velocity, p.rotation_velocity, p.size_x, p.size_y, p.center_offset_min,
            p.center_offset_max, p.rotation_range, p.subtextures_x, p.subtextures_y, p.type,
            p.box_size, p.density));
        ++stats.precipitations;
    }
    std::vector<flatbuffers::Offset<wfb::Region>> regions;
    for (const auto& [id, r] : sink.regions()) {
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

    std::vector<flatbuffers::Offset<wfb::Quest>> quests;
    for (const auto& [id, q] : sink.quests()) {
        std::vector<flatbuffers::Offset<wfb::QuestFragment>> fragments;
        for (const auto& f : q.fragments) {
            fragments.push_back(wfb::CreateQuestFragment(builder, f.stage, f.log_entry,
                                                         builder.CreateString(f.function)));
        }
        std::vector<flatbuffers::Offset<wfb::QuestStage>> stages;
        for (const auto& stage : q.stages) {
            std::vector<flatbuffers::Offset<wfb::QuestLogEntry>> log;
            for (const auto& e : stage.log) {
                log.push_back(wfb::CreateQuestLogEntry(builder, e.flags,
                                                       builder.CreateString(e.text),
                                                       e.conditions));
            }
            stages.push_back(wfb::CreateQuestStage(builder, stage.index, stage.flags,
                                                   builder.CreateVector(log)));
        }
        std::vector<flatbuffers::Offset<wfb::QuestObjective>> objectives;
        for (const auto& o : q.objectives) {
            objectives.push_back(wfb::CreateQuestObjective(builder, o.index, o.flags,
                                                           builder.CreateString(o.text),
                                                           builder.CreateVector(o.targets)));
        }
        std::vector<flatbuffers::Offset<wfb::QuestAlias>> aliases;
        for (const auto& a : q.aliases) {
            const auto name = builder.CreateString(a.name);
            const auto scripts = a.scripts.empty() ? 0 : write_scripts(builder, a.scripts);
            stats.scripts += a.scripts.size();
            wfb::QuestAliasBuilder ab(builder);
            ab.add_id(a.id);
            ab.add_name(name);
            ab.add_location(a.location);
            ab.add_flags(a.flags);
            ab.add_forced(a.forced);
            ab.add_unique_actor(a.unique_actor);
            ab.add_external_quest(a.external_quest);
            ab.add_external_alias(a.external_alias);
            ab.add_created_object(a.created_object);
            ab.add_create_at(a.create_at);
            ab.add_conditions(a.conditions);
            ab.add_display_name(a.display_name);
            if (!a.scripts.empty()) {
                ab.add_scripts(scripts);
            }
            aliases.push_back(ab.Finish());
        }
        stats.quest_aliases += q.aliases.size();
        stats.quest_fragments += q.fragments.size();
        stats.scripts += q.scripts.size();
        const auto editor_id = builder.CreateString(q.editor_id);
        const auto name = builder.CreateString(q.name);
        const auto scripts = q.scripts.empty() ? 0 : write_scripts(builder, q.scripts);
        const auto fragment_script = builder.CreateString(q.fragment_script);
        const auto fragments_off = builder.CreateVector(fragments);
        const auto stages_off = builder.CreateVector(stages);
        const auto objectives_off = builder.CreateVector(objectives);
        const auto aliases_off = builder.CreateVector(aliases);
        wfb::QuestBuilder qb(builder);
        qb.add_id(q.id);
        qb.add_editor_id(editor_id);
        qb.add_name(name);
        qb.add_flags(q.flags);
        qb.add_priority(q.priority);
        qb.add_type(q.type);
        qb.add_event(q.event);
        if (!q.scripts.empty()) {
            qb.add_scripts(scripts);
        }
        qb.add_fragment_script(fragment_script);
        qb.add_fragments(fragments_off);
        qb.add_stages(stages_off);
        qb.add_objectives(objectives_off);
        qb.add_aliases(aliases_off);
        quests.push_back(qb.Finish());
        ++stats.quests;
    }
    std::vector<flatbuffers::Offset<wfb::Global>> globals;
    for (const auto& [id, g] : sink.globals()) {
        globals.push_back(wfb::CreateGlobal(builder, g.id, builder.CreateString(g.editor_id),
                                            static_cast<std::uint8_t>(g.kind), g.value));
        ++stats.globals;
    }
    // ---- what actors are built from ----
    const auto strings = [&](const auto& list) {
        std::vector<flatbuffers::Offset<flatbuffers::String>> offsets;
        for (const auto& text : list) {
            offsets.push_back(builder.CreateString(text));
        }
        return builder.CreateVector(offsets);
    };
    // DPLT names an FLST of packages.
    const auto default_packages = [&](const WorldNpc& n) {
        const auto found = sink.form_lists().find(n.default_package_list);
        return found != sink.form_lists().end() ? found->second : std::vector<std::uint32_t>{};
    };
    std::vector<flatbuffers::Offset<wfb::Npc>> npcs;
    for (const auto& [id, n] : sink.npcs()) {
        std::vector<wfb::NpcItem> items;
        for (const auto& [form, count] : n.items) {
            items.emplace_back(form, count);
        }
        std::vector<wfb::NpcFaction> factions;
        for (const auto& [faction, rank] : n.factions) {
            factions.emplace_back(faction, rank);
        }
        npcs.push_back(wfb::CreateNpc(builder, n.id, builder.CreateString(n.editor_id),
                                      builder.CreateString(n.name), n.flags, n.level, n.race,
                                      n.template_form, n.template_flags, n.skin, n.default_outfit,
                                      n.sleeping_outfit, n.height, n.weight,
                                      builder.CreateVector(n.head_parts),
                                      builder.CreateVectorOfStructs(items),
                                      builder.CreateString(n.face_model),
                                      builder.CreateVector(std::vector<float>(n.skin_tone.begin(), n.skin_tone.end())),
                                      builder.CreateVector(n.packages),
                                      builder.CreateVector(default_packages(n)),
                                      builder.CreateVectorOfStructs(factions)));
        ++stats.npcs;
    }
    const auto conditions = [&](const std::vector<record::Condition>& list) {
        std::vector<flatbuffers::Offset<wfb::Condition>> offsets;
        for (const auto& c : list) {
            offsets.push_back(wfb::CreateCondition(
                builder, c.type, c.function, c.value, c.value_global.value, c.param1, c.param2,
                c.run_on, c.reference.value, c.param3,
                c.string1.empty() ? 0 : builder.CreateString(c.string1),
                c.string2.empty() ? 0 : builder.CreateString(c.string2)));
        }
        return builder.CreateVector(offsets);
    };
    std::vector<flatbuffers::Offset<wfb::Package>> packages;
    for (const auto& [id, p] : sink.packages()) {
        std::vector<flatbuffers::Offset<wfb::PackageInput>> inputs;
        for (const auto& in : p.inputs) {
            inputs.push_back(wfb::CreatePackageInput(
                builder, in.key, builder.CreateString(in.type),
                in.name.empty() ? 0 : builder.CreateString(in.name), in.number, in.location.type,
                in.location.value, in.location.radius, in.target.type, in.target.value,
                in.target.count));
        }
        std::vector<flatbuffers::Offset<wfb::PackageBranch>> branches;
        for (const auto& b : p.branches) {
            branches.push_back(wfb::CreatePackageBranch(
                builder, builder.CreateString(b.type), conditions(b.conditions), b.children,
                b.flags, b.procedure.empty() ? 0 : builder.CreateString(b.procedure),
                b.success_completes, builder.CreateVector(b.inputs), b.set_flags, b.clear_flags,
                b.speed));
        }
        const auto& sch = p.schedule;
        packages.push_back(wfb::CreatePackage(
            builder, p.id, builder.CreateString(p.editor_id), p.type, p.flags,
            p.interrupt_override, p.speed, p.interrupt_flags, sch.month, sch.day_of_week,
            sch.date, sch.hour, sch.minute, sch.duration, conditions(p.conditions),
            p.template_package, builder.CreateVector(inputs), builder.CreateVector(branches),
            p.idle_flags, p.idle_timer, builder.CreateVector(p.idles), p.owner_quest,
            p.combat_style, p.on_begin_idle, p.on_end_idle, p.on_change_idle));
        ++stats.packages;
    }
    std::vector<flatbuffers::Offset<wfb::Race>> races;
    for (const auto& [id, r] : sink.races()) {
        std::vector<flatbuffers::Offset<wfb::RaceBodyPart>> parts;
        for (const auto& p : r.body_parts) {
            parts.push_back(wfb::CreateRaceBodyPart(builder, p.female, p.index, builder.CreateString(p.model)));
        }
        const std::vector<float> heights(r.heights.begin(), r.heights.end());
        const std::vector<float> weights(r.weights.begin(), r.weights.end());
        races.push_back(wfb::CreateRace(builder, r.id, builder.CreateString(r.editor_id),
                                        strings(r.skeletons), strings(r.behaviours), r.skin,
                                        builder.CreateVector(heights), builder.CreateVector(weights),
                                        r.flags, builder.CreateVector(parts),
                                        builder.CreateVector(r.head_parts[0]),
                                        builder.CreateVector(r.head_parts[1]), r.armor_race));
        ++stats.races;
    }
    std::vector<flatbuffers::Offset<wfb::Armor>> armors;
    for (const auto& [id, a] : sink.armors()) {
        armors.push_back(wfb::CreateArmor(builder, a.id, builder.CreateString(a.editor_id), a.slots,
                                          a.race, builder.CreateVector(a.addons)));
        ++stats.armors;
    }
    std::vector<flatbuffers::Offset<wfb::ArmorAddon>> addons;
    for (const auto& [id, a] : sink.armor_addons()) {
        addons.push_back(wfb::CreateArmorAddon(
            builder, a.id, builder.CreateString(a.editor_id), a.slots, a.race,
            builder.CreateVector(a.additional_races), builder.CreateString(a.models[0]),
            builder.CreateString(a.models[1]), a.priorities[0], a.priorities[1], a.weight_sliders[0],
            a.weight_sliders[1]));
        ++stats.armor_addons;
    }
    std::vector<flatbuffers::Offset<wfb::Outfit>> outfits;
    for (const auto& [id, o] : sink.outfits()) {
        outfits.push_back(wfb::CreateOutfit(builder, o.id, builder.CreateVector(o.items)));
        ++stats.outfits;
    }
    std::vector<flatbuffers::Offset<wfb::LeveledList>> leveled;
    for (const auto& [id, l] : sink.leveled_lists()) {
        std::vector<wfb::LeveledEntry> entries;
        for (const auto& e : l.entries) {
            entries.emplace_back(e.level, e.count, e.form);
        }
        leveled.push_back(wfb::CreateLeveledList(builder, l.id, l.type, l.flags, l.chance_none,
                                                 builder.CreateVectorOfStructs(entries)));
        ++stats.leveled_lists;
    }
    const auto npcs_off = builder.CreateVector(npcs);
    const auto packages_off = builder.CreateVector(packages);
    const auto image_spaces_off = builder.CreateVector(image_spaces);
    const auto material_objects_off = builder.CreateVector(material_objects);
    std::vector<flatbuffers::Offset<wfb::Grass>> grasses;
    for (const auto& [id, g] : sink.grasses()) {
        grasses.push_back(wfb::CreateGrass(builder, g.id, builder.CreateString(g.editor_id),
                                           builder.CreateString(g.model), g.density, g.min_slope,
                                           g.max_slope, g.units_from_water, g.water_type, g.position_range,
                                           g.height_range, g.color_range, g.wave_period, g.flags));
    }
    const auto grasses_off = builder.CreateVector(grasses);
    std::vector<flatbuffers::Offset<wfb::AddonNode>> addon_nodes;
    for (const auto& [id, a] : sink.addons()) {
        addon_nodes.push_back(wfb::CreateAddonNode(builder, a.id, builder.CreateString(a.editor_id), a.index,
                                                   builder.CreateString(a.model)));
    }
    const auto addon_nodes_off = builder.CreateVector(addon_nodes);
    const auto races_off = builder.CreateVector(races);
    const auto armors_off = builder.CreateVector(armors);
    const auto addons_off = builder.CreateVector(addons);
    const auto outfits_off = builder.CreateVector(outfits);
    const auto leveled_off = builder.CreateVector(leveled);

    std::vector<flatbuffers::Offset<wfb::Plugin>> plugins;
    for (const auto& entry : order.entries()) {
        if (entry.active) {
            plugins.push_back(wfb::CreatePlugin(builder, builder.CreateString(entry.name),
                                                entry.form_prefix(), entry.is_light));
        }
    }
    const auto plugins_off = builder.CreateVector(plugins);
    const auto quests_off = builder.CreateVector(quests);
    const auto globals_off = builder.CreateVector(globals);
    const auto actors_off = sink.places().write_actors(builder);
    const auto cells_off = builder.CreateVector(cells);
    const auto bases_off = builder.CreateVector(bases);
    const auto climates_off = builder.CreateVector(climates);
    const auto weathers_off = builder.CreateVector(weathers);
    const auto precipitations_off = builder.CreateVector(precipitations);
    const auto regions_off = builder.CreateVector(regions);
    const auto worlds_off = builder.CreateVector(worlds);
    const auto waters_off = builder.CreateVector(waters);
    const auto land_textures_off = builder.CreateVector(land_textures);
    wfb::WorldBuilder wb(builder);
    wb.add_format_version(k_world_format_version);
    wb.add_worlds(worlds_off);
    wb.add_land_textures(land_textures_off);
    wb.add_waters(waters_off);
    wb.add_climates(climates_off);
    wb.add_weathers(weathers_off);
    wb.add_cells(cells_off);
    wb.add_bases(bases_off);
    wb.add_quests(quests_off);
    wb.add_globals(globals_off);
    wb.add_actors(actors_off);
    wb.add_plugins(plugins_off);
    wb.add_precipitations(precipitations_off);
    wb.add_regions(regions_off);
    wb.add_npcs(npcs_off);
    wb.add_packages(packages_off);
    wb.add_image_spaces(image_spaces_off);
    wb.add_material_objects(material_objects_off);
    wb.add_grasses(grasses_off);
    wb.add_addon_nodes(addon_nodes_off);
    wb.add_races(races_off);
    wb.add_armors(armors_off);
    wb.add_armor_addons(addons_off);
    wb.add_outfits(outfits_off);
    wb.add_leveled_lists(leveled_off);
    wfb::FinishWorldBuffer(builder, wb.Finish());

    const std::span<const std::uint8_t> buffer(builder.GetBufferPointer(), builder.GetSize());
    std::string error;
    if (!io::write_file(out, std::as_bytes(buffer), error)) {
        return std::unexpected(io::ParseError{.origin = out.string(),
                                              .offset = 0,
                                              .kind = io::ErrorKind::corrupt,
                                              .detail = "cannot write: " + error});
    }
    stats.file_bytes = buffer.size();
    return stats;
}

} // namespace bethconv::pack
