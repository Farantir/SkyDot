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
#include "world/ai.hpp"
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
          actors_(shared_),
          ai_(shared_) {}

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
        } else if (merged.type == FourCC{"PACK"} || merged.type == FourCC{"FLST"}) {
            ai_.collect(merged, data, form_ctx);
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
    [[nodiscard]] detail::BaseCollector& bases() noexcept { return bases_; }
    [[nodiscard]] detail::EnvironmentCollector& environment() noexcept { return environment_; }
    [[nodiscard]] detail::QuestCollector& quests() noexcept { return quests_; }
    [[nodiscard]] detail::ActorCollector& actors() noexcept { return actors_; }
    [[nodiscard]] detail::AiCollector& ai() noexcept { return ai_; }

private:
    // ---- what actors are built from -------------------------------------

    detail::CollectContext shared_;
    detail::PlaceCollector places_;
    detail::BaseCollector bases_;
    detail::EnvironmentCollector environment_;
    detail::QuestCollector quests_;
    detail::ActorCollector actors_;
    detail::AiCollector ai_;
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
    const auto npcs = sink.actors().write_npcs(builder, sink.ai().form_lists());
    const auto packages = sink.ai().write_packages(builder);
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
