// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/world.hpp"

#include "bethconv/io/span_stream.hpp"

#include "world/sink.hpp"

#include "bethconv/pack/world_generated.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bethconv::pack {
namespace {

/// The tables of `list` packed from their objects, in a vector of their own.
template <typename T>
auto pack_all(flatbuffers::FlatBufferBuilder& builder,
              const std::vector<std::unique_ptr<T>>& list) {
    std::vector<flatbuffers::Offset<typename T::TableType>> offsets;
    offsets.reserve(list.size());
    for (const auto& table : list) {
        offsets.push_back(T::TableType::Pack(builder, table.get()));
    }
    return builder.CreateVector(offsets);
}

} // namespace

io::ParseResult<WorldStats> write_world(const record::MergedWorld& world,
                                        const record::LoadOrder& order,
                                        const std::filesystem::path& out) {
    detail::WorldSink sink(order);
    world.for_each_record(sink);
    auto& stats = sink.stats();

    wfb::WorldT root;
    sink.places().finish(root);
    sink.bases().finish(root);
    sink.environment().finish(root);
    sink.quests().finish(root);
    sink.actors().finish(root, sink.ai().form_lists());
    for (const auto& entry : order.entries()) {
        if (entry.active) {
            auto& plugin = root.plugins.emplace_back(std::make_unique<wfb::PluginT>());
            plugin->name = entry.name;
            plugin->prefix = entry.form_prefix();
            plugin->light = entry.is_light;
        }
    }

    // Every table of the root is packed from its object, in this order: the
    // order tables, strings and vectors are created in is the order they lie in
    // the file, so it is what makes the bytes reproducible. Only the packages
    // are written by hand (see AiCollector).
    flatbuffers::FlatBufferBuilder builder(1u << 20);
    const auto cells = pack_all(builder, root.cells);
    const auto bases = pack_all(builder, root.bases);
    const auto worlds = pack_all(builder, root.worlds);
    const auto land_textures = pack_all(builder, root.land_textures);
    const auto waters = pack_all(builder, root.waters);
    const auto climates = pack_all(builder, root.climates);
    const auto weathers = pack_all(builder, root.weathers);
    const auto image_spaces = pack_all(builder, root.image_spaces);
    const auto material_objects = pack_all(builder, root.material_objects);
    const auto precipitations = pack_all(builder, root.precipitations);
    const auto regions = pack_all(builder, root.regions);
    const auto quests = pack_all(builder, root.quests);
    const auto globals = pack_all(builder, root.globals);
    const auto npcs = pack_all(builder, root.npcs);
    const auto packages = sink.ai().write_packages(builder);
    const auto races = pack_all(builder, root.races);
    const auto armors = pack_all(builder, root.armors);
    const auto armor_addons = pack_all(builder, root.armor_addons);
    const auto outfits = pack_all(builder, root.outfits);
    const auto leveled_lists = pack_all(builder, root.leveled_lists);
    const auto grasses = pack_all(builder, root.grasses);
    const auto addon_nodes = pack_all(builder, root.addon_nodes);
    const auto plugins = pack_all(builder, root.plugins);
    const auto actors = builder.CreateVectorOfStructs(root.actors);
    wfb::FinishWorldBuffer(
        builder, wfb::CreateWorld(builder, k_world_format_version, worlds, land_textures, waters,
                                  climates, weathers, cells, bases, quests, globals, actors,
                                  plugins, precipitations, regions, npcs, races, armors,
                                  armor_addons, outfits, leveled_lists, packages, image_spaces,
                                  material_objects, grasses, addon_nodes));

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
