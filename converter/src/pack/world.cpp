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

#include "world/actors.hpp"
#include "world/bases.hpp"
#include "world/context.hpp"
#include "world/environment.hpp"
#include "world/fb_write.hpp"
#include "world/places.hpp"
#include "world/quests.hpp"

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
        : shared_(order),
          places_(shared_),
          bases_(shared_),
          environment_(shared_),
          quests_(shared_),
          actors_(shared_) {}

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
        } else if (merged.type == FourCC{"QUST"} || merged.type == FourCC{"GLOB"}) {
            quests_.collect(merged, data, form_ctx);
        } else if (merged.type == FourCC{"RACE"} || merged.type == FourCC{"ARMA"} ||
                   merged.type == FourCC{"OTFT"} || merged.type == FourCC{"LVLI"}) {
            actors_.collect(merged, data, form_ctx);
        } else if (merged.type == FourCC{"PACK"}) {
            on_package(merged, data, form_ctx);
        } else if (merged.type == FourCC{"FLST"}) {
            on_form_list(merged, data, form_ctx);
        } else if (merged.type != FourCC{"INFO"}) {
            // These are bases too (placed armor, scripted NPCs); read twice.
            io::SpanReader copy = data;
            if (merged.type == FourCC{"NPC_"} || merged.type == FourCC{"ARMO"} ||
                merged.type == FourCC{"LVLN"}) {
                actors_.collect(merged, copy, form_ctx);
            }
            bases_.collect_generic(merged, data);
        }
    }

    [[nodiscard]] WorldStats& stats() noexcept { return shared_.stats(); }
    [[nodiscard]] detail::PlaceCollector& places() noexcept { return places_; }
    [[nodiscard]] std::map<std::uint32_t, WorldPackage>& packages() noexcept { return packages_; }
    [[nodiscard]] const std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>& form_lists()
        const noexcept {
        return form_lists_;
    }
    [[nodiscard]] detail::BaseCollector& bases() noexcept { return bases_; }
    [[nodiscard]] detail::EnvironmentCollector& environment() noexcept { return environment_; }
    [[nodiscard]] detail::QuestCollector& quests() noexcept { return quests_; }
    [[nodiscard]] detail::ActorCollector& actors() noexcept { return actors_; }

private:
    // ---- what actors are built from -------------------------------------

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

    detail::CollectContext shared_;
    detail::PlaceCollector places_;
    detail::BaseCollector bases_;
    detail::EnvironmentCollector environment_;
    detail::QuestCollector quests_;
    detail::ActorCollector actors_;
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

    const auto quests = sink.quests().write_quests(builder);
    const auto globals = sink.quests().write_globals(builder);
    // ---- what actors are built from ----
    const auto npcs = sink.actors().write_npcs(builder, sink.form_lists());
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
    const auto races = sink.actors().write_races(builder);
    const auto armors = sink.actors().write_armors(builder);
    const auto addons = sink.actors().write_armor_addons(builder);
    const auto outfits = sink.actors().write_outfits(builder);
    const auto leveled = sink.actors().write_leveled_lists(builder);
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
