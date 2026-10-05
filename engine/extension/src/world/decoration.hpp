// SPDX-License-Identifier: GPL-3.0-or-later
//
// `Decorator`: what is done to a model once it is instanced, in one fixed
// order (the order matters: collision comes last so no pass before it sees the
// bodies, the animator after the materials it takes over):
//
//   drop the NIF root's transform, draw order, Skyrim materials, water
//   material, directional material, add-on nodes, billboards, animators, tags
//   for activation, plain-door mark, collision.
//
// The steps and their order are the table in `Decorator::decorate`. An add-on's model
// (a candle's flame) goes through the same list without the steps that only
// make sense for a placed reference. Each step checks its own setting
// (BuildOptions), so the list is the same whatever is switched on.
//
// The Decorator also keeps what the steps share between models: the materials,
// the water materials, the projected materials and the add-on index.
#pragma once

#include "assets/asset_cache.hpp"
#include "world/build_options.hpp"
#include "world/materials.hpp"
#include "world/water.hpp"
#include "world/world_data.hpp"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace bethconv::pack::wfb {
struct Base;
struct Ref;
struct Water;
} // namespace bethconv::pack::wfb

namespace skydot {

class SkydotModel;

/// What the steps count, per model or per cell.
struct DecorationStats {
    std::int64_t materials = 0;
    std::int64_t billboards = 0;
    std::int64_t effects = 0;
    std::int64_t bodies = 0;
};

/// The model being decorated and what it stands for.
struct Decoration {
    DecorationStats& stats;
    /// The reference the model is placed for, its base and its cell; null for
    /// an add-on's model, which takes the steps that fit any model.
    const bethconv::pack::wfb::Ref* ref{};
    const bethconv::pack::wfb::Base* base{};
    std::uint32_t cell{};
    /// What the node was instanced from, for its collision.
    const SkydotModel* scene{};
};

class Decorator {
public:
    /// `options` is the builder's own and must outlive the decorator.
    Decorator(std::shared_ptr<const WorldData> data, const BuildOptions& options);
    Decorator(const Decorator&) = delete;
    Decorator& operator=(const Decorator&) = delete;

    /// Switch to the opened world.fb (as CellBuilder::open).
    void open(std::shared_ptr<const WorldData> data);
    void set_assets(std::shared_ptr<AssetCache> assets);

    /// Run the steps on `node`, a freshly instanced model.
    void decorate(godot::Node3D* node, Decoration& decoration);

    /// The materials every model is converted with; created on first use and
    /// attached to the asset cache.
    SkydotMaterials& materials();
    /// A resource from the cache, else loaded now (and cached).
    godot::Ref<godot::Resource> resource(const godot::String& vpath) const;
    /// The material of a water type (null for a default look), shared by
    /// placed water and the exterior's water plane.
    godot::Ref<godot::ShaderMaterial> water_material(const bethconv::pack::wfb::Water* water);
    /// Compile the material and water shader variants now. Returns how many.
    std::int64_t warm_up();

private:
    struct Steps;
    friend struct Steps;

    /// MATO `id` as the lighting shader takes it; null if there is none.
    const ProjectedMaterial* projected_material(std::uint32_t id);
    /// Attach the ADDN models (candle flames, smoke) to `model`'s AddOnNodes
    /// (mesh extras "addon"). Returns how many.
    std::int64_t attach_addons(godot::Node* model);

    std::shared_ptr<const WorldData> data_;
    const BuildOptions& options_;
    std::shared_ptr<AssetCache> assets_;
    godot::Ref<SkydotMaterials> materials_;
    WaterMaterials water_;
    std::mutex projected_mutex_;
    std::unordered_map<std::uint32_t, std::optional<ProjectedMaterial>> projected_;
    std::once_flag addon_index_once_;
    std::unordered_map<std::int32_t, std::string> addon_models_;
};

} // namespace skydot
