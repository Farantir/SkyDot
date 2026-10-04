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
#include "bethconv/record/vmad.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bethconv::pack::detail {

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
    wfb::GrassFlags flags{};
};

struct BaseEntry {
    std::uint32_t id{};
    std::uint32_t type{};
    std::string editor_id;
    std::string model;
    std::optional<WorldLight> light;
    std::uint32_t flags{};
    std::vector<record::Script> scripts;
    wfb::RecordFlags record_flags{};
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

class BaseCollector {
public:
    explicit BaseCollector(CollectContext& shared) : shared_(shared) {}

    /// LIGH, MATO, ADDN, TXST, LTEX or GRAS; nothing for another type.
    void collect(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx);

    /// Any other type: kept as a base if it has a model or scripts.
    void collect_generic(const record::MergedRecord& merged, io::SpanReader& data);

    /// The bases, in id order.
    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::Base>> write_bases(
        flatbuffers::FlatBufferBuilder& builder);

    /// The land textures with their texture set's paths, in id order.
    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::LandTexture>> write_land_textures(
        flatbuffers::FlatBufferBuilder& builder);

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::MaterialObject>> write_material_objects(
        flatbuffers::FlatBufferBuilder& builder);

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::Grass>> write_grasses(
        flatbuffers::FlatBufferBuilder& builder);

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::AddonNode>> write_addon_nodes(
        flatbuffers::FlatBufferBuilder& builder);

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
    std::map<std::uint32_t, BaseEntry> bases_;
    std::map<std::uint32_t, MaterialObjectEntry> material_objects_;
    std::map<std::uint32_t, GrassEntry> grasses_;
    std::map<std::uint32_t, AddonEntry> addons_;
    std::map<std::uint32_t, LandTextureEntry> land_textures_;
    std::unordered_map<std::uint32_t, TextureSetEntry> texture_sets_;
};

} // namespace bethconv::pack::detail
