// SPDX-License-Identifier: GPL-3.0-or-later
//
// What cells place: LIGH, the other types that have a model or scripts, MATO,
// ADDN, GRAS, and the land textures with the texture sets they name. Private
// to pack/world/.
#pragma once

#include "context.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/pack/world.hpp"
#include "bethconv/pack/world_generated.h"
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/merge.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace bethconv::pack::detail {

struct TextureSetEntry {
    std::string diffuse;
    std::string normal;
};

/// An LTEX: the table, and the TXST it names, whose paths fill the table in when
/// every record has been read.
struct LandTextureEntry {
    wfb::LandTextureT table;
    std::uint32_t texture_set{};
};

class BaseCollector {
public:
    explicit BaseCollector(CollectContext& shared) : shared_(shared) {}

    /// LIGH, MATO, ADDN, TXST, LTEX or GRAS; nothing for another type.
    void collect(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx);

    /// Any other type: kept as a base if it has a model or scripts.
    void collect_generic(const record::MergedRecord& merged, io::SpanReader& data);

    /// The bases, land textures (with their texture set's paths), material
    /// objects, grasses and addon nodes, each in id order, into `world`.
    void finish(wfb::WorldT& world);

private:
    void on_light(const record::MergedRecord& merged, io::SpanReader& data,
                  const record::FormContext& form_ctx);
    void on_material_object(const record::MergedRecord& merged, io::SpanReader& data);
    void on_land_texture(const record::MergedRecord& merged, io::SpanReader& data,
                         const record::FormContext& form_ctx);
    void on_addon_node(const record::MergedRecord& merged, io::SpanReader& data);
    void on_grass(const record::MergedRecord& merged, io::SpanReader& data);
    void on_texture_set(const record::MergedRecord& merged, io::SpanReader& data,
                        const record::FormContext& form_ctx);

    CollectContext& shared_;
    std::map<std::uint32_t, wfb::BaseT> bases_;
    std::map<std::uint32_t, wfb::MaterialObjectT> material_objects_;
    std::map<std::uint32_t, wfb::GrassT> grasses_;
    std::map<std::uint32_t, wfb::AddonNodeT> addons_;
    std::map<std::uint32_t, LandTextureEntry> land_textures_;
    std::unordered_map<std::uint32_t, TextureSetEntry> texture_sets_;
};

} // namespace bethconv::pack::detail
