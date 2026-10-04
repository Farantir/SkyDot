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

#include "world/bases.hpp"
#include "world/context.hpp"
#include "world/environment.hpp"
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

class WorldSink final : public record::MergedRecordSink {
public:
    explicit WorldSink(const record::LoadOrder& order)
        : shared_(order), places_(shared_), bases_(shared_), environment_(shared_) {}

    void on_record(const record::MergedRecord& merged, const record::RecordContext& ctx,
                   io::SpanReader& data, const record::FormContext& form_ctx) override {
        if (merged.deleted) {
            return;
        }
        if (merged.type == FourCC{"CELL"} || merged.type == FourCC{"REFR"} ||
            merged.type == FourCC{"ACHR"} || merged.type == FourCC{"LAND"} ||
            merged.type == FourCC{"NAVM"} || merged.type == FourCC{"LGTM"}) {
            places_.collect(merged, ctx, data, form_ctx);
        } else if (merged.type == FourCC{"LIGH"} || merged.type == FourCC{"MATO"} ||
                   merged.type == FourCC{"ADDN"} || merged.type == FourCC{"TXST"} ||
                   merged.type == FourCC{"LTEX"} || merged.type == FourCC{"GRAS"}) {
            bases_.collect(merged, data, form_ctx);
        } else if (merged.type == FourCC{"WRLD"} || merged.type == FourCC{"WATR"} ||
                   merged.type == FourCC{"CLMT"} || merged.type == FourCC{"WTHR"} ||
                   merged.type == FourCC{"SPGD"} || merged.type == FourCC{"REGN"} ||
                   merged.type == FourCC{"IMGS"}) {
            environment_.collect(merged, data, form_ctx);
        } else if (merged.type == FourCC{"QUST"}) {
            on_quest(merged, data, form_ctx);
        } else if (merged.type == FourCC{"GLOB"}) {
            on_global(merged, data, form_ctx);
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
            bases_.collect_generic(merged, data);
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
    [[nodiscard]] detail::BaseCollector& bases() noexcept { return bases_; }
    [[nodiscard]] detail::EnvironmentCollector& environment() noexcept { return environment_; }
    [[nodiscard]] std::map<std::uint32_t, WorldQuest>& quests() noexcept { return quests_; }
    [[nodiscard]] std::map<std::uint32_t, WorldGlobal>& globals() noexcept { return globals_; }

private:
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

    detail::CollectContext shared_;
    detail::PlaceCollector places_;
    detail::BaseCollector bases_;
    detail::EnvironmentCollector environment_;
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

    const auto bases = sink.bases().write_bases(builder);

    const auto worlds = sink.environment().write_worlds(builder);

    const auto land_textures = sink.bases().write_land_textures(builder);

    const auto waters = sink.environment().write_waters(builder);
    const auto climates = sink.environment().write_climates(builder);
    const auto weathers = sink.environment().write_weathers(builder);
    const auto image_spaces = sink.environment().write_image_spaces(builder);
    const auto material_objects = sink.bases().write_material_objects(builder);
    const auto precipitations = sink.environment().write_precipitations(builder);
    const auto regions = sink.environment().write_regions(builder);

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
    const auto grasses = sink.bases().write_grasses(builder);
    const auto grasses_off = builder.CreateVector(grasses);
    const auto addon_nodes = sink.bases().write_addon_nodes(builder);
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
