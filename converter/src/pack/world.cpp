// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/world.hpp"

#include "bethconv/io/span_stream.hpp"

#include "world/sink.hpp"

#include "bethconv/pack/world_generated.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bethconv::pack {

io::ParseResult<WorldStats> write_world(const record::MergedWorld& world,
                                        const record::LoadOrder& order,
                                        const std::filesystem::path& out) {
    detail::WorldSink sink(order);
    world.for_each_record(sink);
    auto& stats = sink.stats();

    flatbuffers::FlatBufferBuilder builder(1u << 20);

    // The builder lays bytes out in the order tables, strings and vectors are
    // created, so this sequence (the write_* calls and the CreateVector calls
    // that follow them) is what keeps world.fb's bytes stable.
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
