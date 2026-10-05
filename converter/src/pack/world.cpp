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

io::ParseResult<WorldStats> write_world(const record::MergedWorld& world,
                                        const record::LoadOrder& order,
                                        const std::filesystem::path& out) {
    detail::WorldSink sink(order);
    world.for_each_record(sink);
    auto& stats = sink.stats();

    wfb::WorldT root;
    root.format_version = k_world_format_version;
    sink.places().finish(root);
    sink.bases().finish(root);
    sink.environment().finish(root);
    sink.quests().finish(root);
    sink.actors().finish(root, sink.ai().form_lists());
    sink.ai().finish(root);
    for (const auto& entry : order.entries()) {
        if (entry.active) {
            auto& plugin = root.plugins.emplace_back(std::make_unique<wfb::PluginT>());
            plugin->name = entry.name;
            plugin->prefix = entry.form_prefix();
            plugin->light = entry.is_light;
        }
    }

    flatbuffers::FlatBufferBuilder builder(1u << 20);
    wfb::FinishWorldBuffer(builder, wfb::World::Pack(builder, &root));

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
