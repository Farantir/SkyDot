// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotLod`: a worldspace's distant land, objects and trees from the game's
// LOD (formats/pack-format.md, "LOD").
//
// The LOD grid is split into quads of 4, 8, 16 and 32 cells. `update` walks
// it as a quadtree from the coarsest level down, splitting a quad while the
// camera is closer than `split_distance` times its size, and shows the
// terrain (.btr) and objects (.bto) of the quads it ends on, and tree
// billboards (.btt) for level-4 quads within `tree_distance`. A quad stays up
// until what replaces it is built, so nothing flickers while loading.
//
// Where full-detail cells are loaded (`set_cell_loaded`), LOD must not draw:
// every LOD shader discards fragments over cells marked in a per-cell mask.
// Where large references are drawn as models (SkydotLargeRefs, a child), only
// the object shapes the game's LOD made of them (named ...LargeRef) are
// hidden: the mask has a second state for that.
#pragma once

#include "assets/pack.hpp"
#include "world/world.hpp"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace skydot {

class SkydotLargeRefs;

class SkydotLod : public godot::Node3D {
    GDCLASS(SkydotLod, godot::Node3D)

public:
    /// Read `world`'s LOD settings and tree list from `pack`. Meshes come
    /// from the pack's asset cache. Fails if the worldspace (or the one whose land
    /// it uses) has no LOD settings.
    godot::Error setup(const godot::Ref<SkydotPack>& pack, const godot::Ref<SkydotWorld>& world,
                       std::int64_t world_id);
    godot::String get_error() const;

    /// Choose quads for a camera at `camera` (Godot space), start loading
    /// what they need, build what has loaded (for at most `budget_usec`) and
    /// drop what is replaced. Returns how many wanted quads are not built yet.
    std::int64_t update(const godot::Vector3& camera, std::int64_t budget_usec);

    /// Full-detail cell (x, y) is built (or dropped): LOD hides there.
    void set_cell_loaded(std::int64_t x, std::int64_t y, bool loaded);
    void clear_loaded_cells();
    /// Large references are all drawn as models for the cells around (x, y),
    /// or no longer: the object shapes made of large references hide there.
    void set_cell_large_refs(std::int64_t x, std::int64_t y, bool drawn);

    /// Draw the worldspace's large references (pack format 11) out to
    /// `large_ref_radius` cells from the camera, beyond the cells at full
    /// detail. On by default where the pack has them; radius 5 by default.
    void set_large_refs(bool enabled);
    bool get_large_refs() const { return large_refs_enabled_; }
    void set_large_ref_radius(std::int64_t cells);
    std::int64_t get_large_ref_radius() const { return large_ref_radius_; }
    /// The layer, or null (off, or the worldspace has none).
    SkydotLargeRefs* get_large_ref_layer() const { return large_refs_; }

    /// A quad of L cells splits while the camera is within this many times L
    /// cells of it. Default 1.5.
    void set_split_distance(double factor);
    double get_split_distance() const;
    /// Trees show for level-4 quads within this many cells. Default 16.
    void set_tree_distance(double cells);
    double get_tree_distance() const;

    /// levels (level -> quads shown), objects, tree_quads, trees, pending,
    /// loaded_cells, and large_refs (SkydotLargeRefs::get_stats) when on.
    godot::Dictionary get_stats() const;

    /// The code of every LOD shader, by name.
    static godot::Dictionary shader_codes();

protected:
    static void _bind_methods();

private:
    struct Quad {
        int level = 0;
        int x = 0; ///< South-west cell.
        int y = 0;
        bool operator<(const Quad& o) const {
            return std::tie(level, x, y) < std::tie(o.level, o.x, o.y);
        }
        bool operator==(const Quad& o) const = default;
        [[nodiscard]] bool overlaps(const Quad& o) const {
            return x < o.x + o.level && o.x < x + level && y < o.y + o.level && o.y < y + level;
        }
    };
    struct Shown {
        godot::Node3D* node = nullptr;
        std::size_t trees = 0;
        bool objects = false;
    };

    void select(const Quad& q, double cx, double cy, std::set<Quad>& out) const;
    /// Resource paths a quad needs (terrain, objects).
    std::vector<godot::String> scenes_for(const Quad& q) const;
    /// Start or poll a threaded load. True once it has finished (or failed).
    bool loaded(const godot::String& path);
    godot::Node3D* build(const Quad& q, bool trees, Shown& stats);
    void add_trees(godot::Node3D* parent, const Quad& q, Shown& stats);
    void retexture(godot::Node* node, bool terrain, bool water, bool large_ref = false);
    godot::Ref<godot::ShaderMaterial> material(const godot::Ref<godot::Shader>& shader,
                                               const godot::Ref<godot::Texture2D>& albedo,
                                               const godot::Ref<godot::Texture2D>& normal, bool large_ref = false);
    void apply_mask(const godot::Ref<godot::ShaderMaterial>& m) const;
    [[nodiscard]] godot::String vpath(const Quad& q, const char* kind) const;

    godot::Ref<SkydotPack> pack_;
    godot::Ref<SkydotWorld> world_;
    godot::String name_;
    godot::String error_;
    int south_west_x_ = 0;
    int south_west_y_ = 0;
    int stride_ = 0;
    int lowest_ = 4;
    int highest_ = 32;
    double split_distance_ = 1.5;
    double tree_distance_ = 16.0;

    struct TreeType {
        float width = 0.0F;
        float height = 0.0F;
        float u0 = 0.0F, v0 = 0.0F, u1 = 0.0F, v1 = 0.0F;
    };
    std::unordered_map<std::uint32_t, TreeType> tree_types_;
    godot::Ref<godot::Texture2D> tree_atlas_;
    /// textures/terrain/noise.dds, the detail laid over terrain LOD; null without it.
    godot::Ref<godot::Texture2D> noise_;

    std::map<Quad, Shown> shown_;
    /// Threaded loads: path -> resource, null once failed; absent while not
    /// requested.
    std::unordered_map<std::string, godot::Ref<godot::Resource>> resources_;
    std::set<std::string> pending_; ///< Requested from the asset cache, not loaded yet.
    /// Vpaths the pack lacks, so their quads count as built.
    mutable std::unordered_map<std::string, bool> exists_;

    /// What the mask shows of cell (x, y): 1 full detail built, 0.5 large
    /// references drawn, else 0. Kept per cell so each can change on its own.
    void write_mask(std::int32_t px, std::int32_t py);
    void make_large_refs();
    std::vector<std::uint8_t> mask_flags_; ///< bit 0 loaded, bit 1 large references drawn.
    SkydotLargeRefs* large_refs_ = nullptr;
    bool large_refs_enabled_ = true;
    std::int64_t large_ref_radius_ = 5;
    std::int64_t large_ref_budget_usec_ = 2000; ///< Per update for building large references.
    std::int64_t world_id_ = 0;
    godot::Ref<godot::Image> mask_image_;
    godot::Ref<godot::ImageTexture> mask_;
    bool mask_dirty_ = false;
    std::size_t loaded_cells_ = 0;

    godot::Ref<godot::Shader> terrain_shader_;
    godot::Ref<godot::Shader> object_shader_;
    godot::Ref<godot::Shader> water_shader_;
    godot::Ref<godot::Shader> tree_shader_;
    godot::Ref<godot::ShaderMaterial> water_material_;
    godot::Ref<godot::ShaderMaterial> tree_material_;
    godot::Ref<godot::ArrayMesh> tree_quad_;
    std::map<std::pair<std::int64_t, std::int64_t>, godot::Ref<godot::ShaderMaterial>> materials_;
};

} // namespace skydot
