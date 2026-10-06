// SPDX-License-Identifier: GPL-3.0-or-later
//
// `CellBuilder`: builds the scene of a cell from a WorldData: its models and
// lights, and for an exterior cell terrain, water and grass, plus navmeshes
// and the actors standing there. A build is a job, advanced in steps within a
// time budget so that a large cell never stalls a frame.
//
// The builder owns what building keeps between cells (jobs under way, the
// terrain builder, materials, water, grass and projected-material caches), so
// building changes it and none of its methods that build are const. Not
// thread-safe, except the two caches the asset workers also reach (grass
// models, projected materials), which are guarded.
#pragma once

#include "assets/asset_cache.hpp"
#include "actors/actor_placement.hpp"
#include "data/actors.hpp"
#include "build/build_options.hpp"
#include "build/decoration.hpp"
#include "render/grass.hpp"
#include "actors/locomotion.hpp"
#include "render/materials.hpp"
#include "render/terrain.hpp"
#include "data/world_data.hpp"

#include <godot_cpp/classes/animation.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bethconv::pack::wfb {
struct Base;
struct Ref;
struct LargeRef;
struct ActorRef;
struct Grass;
} // namespace bethconv::pack::wfb

namespace skydot {

class CellBuilder {
public:
    /// Builds from `data` (a closed WorldData builds nothing; `open` switches
    /// to the opened one). `placement` is where actors are; it must outlive
    /// the builder.
    CellBuilder(std::shared_ptr<const WorldData> data, ActorPlacement& placement);
    CellBuilder(const CellBuilder&) = delete;
    CellBuilder& operator=(const CellBuilder&) = delete;

    /// Switch to the opened world.fb. Once, before anything is built: the
    /// caches point into it.
    void open(std::shared_ptr<const WorldData> data);
    /// The pack's asset cache, which every model and texture comes from;
    /// without it nothing loads.
    void set_assets(std::shared_ptr<AssetCache> assets);
    BuildOptions& options() { return options_; }
    const BuildOptions& options() const { return options_; }

    /// A resource from the cache, else loaded now (and cached).
    godot::Ref<godot::Resource> resource(const godot::String& vpath) const;

    /// Build a cell under a new Node3D, to the end. Null if there is no such
    /// cell (or, for an exterior, neither a cell nor terrain).
    godot::Node3D* build_cell(std::int64_t id);
    godot::Node3D* build_exterior(std::int64_t world, std::int64_t x, std::int64_t y);
    /// The same in steps: navmeshes (and for an exterior terrain, water and
    /// grass) now, references and actors by `continue_build`.
    godot::Node3D* begin_cell(std::int64_t id);
    godot::Node3D* begin_exterior(std::int64_t world, std::int64_t x, std::int64_t y);
    /// Place references of a cell `begin_*` returned until `budget_usec` is
    /// spent. True once all are placed (and "skydot_stats" is set); also true
    /// for a root with no build under way.
    bool continue_build(godot::Node3D* root, std::int64_t budget_usec);
    /// Only the references (models, lights) of such a build; true once they
    /// are placed.
    bool continue_build_static(godot::Node3D* root, std::int64_t budget_usec);
    /// One reference of `cell`, even if it starts disabled. Null if there is
    /// no such reference.
    godot::Node3D* build_ref(std::int64_t cell, std::int64_t ref);
    /// The model path (asset cache key) a large reference draws, or empty if
    /// it draws nothing: initially disabled, no base, no model, or a marker.
    /// `reason` (may be null) gets what kept it out: "disabled", "no base",
    /// "no model", "marker".
    godot::String large_ref_model(const bethconv::pack::wfb::LargeRef& ref, const char** reason = nullptr) const;
    /// A large reference's model, placed and given its materials, for looking
    /// at only: no collision, effects, lights or scripts. Null if its model is
    /// not loaded (see `large_ref_model`).
    godot::Node3D* build_large_ref(const bethconv::pack::wfb::LargeRef& ref);
    /// Placed actor `ref` alone, at its place; null if it has none.
    godot::Node3D* build_actor(std::int64_t ref);

    /// Virtual paths of the resources a cell needs, and a request for them on
    /// the asset cache's threads. A request returns how many are still pending.
    godot::PackedStringArray cell_resources(std::int64_t id);
    godot::PackedStringArray exterior_resources(std::int64_t world, std::int64_t x, std::int64_t y);
    std::int64_t request_cell(std::int64_t id);
    std::int64_t request_exterior(std::int64_t world, std::int64_t x, std::int64_t y);
    /// Loaded resources kept for reuse, and those still loading.
    std::int64_t cached_resource_count() const;
    std::int64_t pending_resource_count() const;
    /// Drop cached resources nothing else holds.
    void trim_cache();
    /// Compile every material and terrain shader variant now. Returns how many.
    std::int64_t warm_up();

    /// Set the shadows of the lights placed under `root`: all of them, or only
    /// those the records flag. Returns how many changed.
    static std::int64_t apply_light_shadows(godot::Node* root, bool all);

    /// The plan for a placed actor, from world.fb and what the pack holds.
    ActorPlan actor_plan(const bethconv::pack::wfb::ActorRef& actor) const;
    /// The clips of a behaviour project; read once, then kept.
    const Locomotion& locomotion_of(const std::string& behaviour);

private:
    struct BuildStats;
    struct BuildJob;

    const WorldData& data() const { return *data_; }
    const bethconv::pack::wfb::World* world_fb() const { return data_->root(); }
    SkydotMaterials& materials() { return decorator_.materials(); }
    /// terrain_, made on first use and again when the tiling changed.
    TerrainBuilder& terrain_builder();
    std::array<std::string, 2> land_texture_paths(std::uint32_t ltex) const;

    void add_job(godot::Node3D* root, std::shared_ptr<BuildJob> job);
    /// Hold what `actors` need, if cached, in `job`, so trimming the cache
    /// before they are placed (leaving a place, cells dropped) keeps it.
    void keep_actor_resources(BuildJob& job, const std::vector<ActorPlacement::ActorAt>& actors);
    /// Place `job`'s references until the budget from `started` is spent.
    bool place_refs(godot::Node3D* root, BuildJob& job, std::uint64_t started, std::int64_t budget_usec);
    /// The model `ref` shows, unless it is disabled or a marker.
    void add_ref_resources(const bethconv::pack::wfb::Ref& ref, godot::PackedStringArray& out) const;
    void add_actor_resources(const std::vector<ActorPlacement::ActorAt>& actors, godot::PackedStringArray& out);
    std::int64_t request_all(const godot::PackedStringArray& paths);
    /// Instance the reference's model and light under `root`.
    void place_ref(godot::Node3D* root, const bethconv::pack::wfb::Ref& ref, std::uint32_t cell,
                   BuildStats& stats, bool include_disabled = false);
    /// A grass's model for instancing (grass.hpp), loaded once.
    GrassModel grass_model(const bethconv::pack::wfb::Grass& grass);
    godot::Dictionary stats_dictionary(const BuildStats& stats) const;

    /// The idle, walk and run clip files an actor of `plan` plays; empty
    /// where it has none.
    struct ActorClips {
        std::string idle;
        std::string walk;
        std::string run;
    };
    ActorClips actor_clips(const ActorPlan& plan);
    /// The looping clip `file` for the skeleton asset `skeleton_path`, from
    /// the asset cache (built on its workers when requested ahead); null if
    /// it does not build.
    godot::Ref<godot::Animation> actor_clip(const std::string& file, const std::string& skeleton_path) const;
    void place_actor(godot::Node3D* root, const bethconv::pack::wfb::ActorRef& actor, BuildStats& stats,
                     const ActorPlacement::Place* place = nullptr);
    /// The actor's models and clips (asset cache keys) to request ahead.
    std::vector<std::string> actor_resources(const bethconv::pack::wfb::ActorRef& actor);

    std::shared_ptr<const WorldData> data_;
    ActorPlacement& placement_;
    std::shared_ptr<AssetCache> assets_;
    BuildOptions options_;

    /// Builds under way, by root instance id.
    std::unordered_map<std::uint64_t, std::shared_ptr<BuildJob>> jobs_;

    /// What is done to every instanced model, and the caches that go with it.
    Decorator decorator_;

    // What building keeps between cells.
    std::unique_ptr<TerrainBuilder> terrain_;
    double terrain_tiling_built_{};
    std::mutex grass_mutex_;
    std::unordered_map<std::uint32_t, GrassModel> grass_models_;
    /// Behaviour project -> its idle, walk and run clips.
    std::unordered_map<std::string, Locomotion> locomotion_;
    /// meshes/animationdatasinglefile.txt, read once for every project it holds.
    godot::PackedByteArray animation_single_file_;
    bool animation_single_file_read_{false};
};

} // namespace skydot
