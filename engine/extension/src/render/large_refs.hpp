// SPDX-License-Identifier: GPL-3.0-or-later
//
// `SkydotLargeRefs`: a worldspace's large references drawn as models between
// the cells at full detail and the LOD (pack format 11, formats/pack-format.md
// "large references"). The game draws, out to uLargeRefLODGridSize (11 cells,
// here `radius`, default 5), the references the Creation Kit lists as large
// (cliffs, rocks, big buildings) as full models, seen only: no collision,
// scripts, lights or actors. Beyond that its object LOD shows them.
//
// Each `update` takes the large references listed for the cells within
// `radius` of the camera (data/large_refs.hpp), minus those standing in a cell
// built at full detail (`set_cell_built`: that cell draws them), builds the
// ones whose models have loaded, nearest first within a budget, and drops
// what is no longer wanted. A reference stays until its own cell is built, so
// moving never opens a hole.
//
// Object LOD already draws about a third of the large references, as shapes
// named ...LargeRef. Where every large reference of a cell is drawn here (or
// is not drawable, or is built by its own cell), the layer tells the LOD
// (`SkydotLod::set_cell_large_refs`) to hide those shapes there.
#pragma once

#include "assets/pack.hpp"
#include "world/world.hpp"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace skydot {

class SkydotLod;

class SkydotLargeRefs : public godot::Node3D {
    GDCLASS(SkydotLargeRefs, godot::Node3D)

public:
    /// For worldspace `world_id`, whose large references `world` holds.
    /// `lod` (may be null) gets the cells to hide LargeRef shapes in. Fails if
    /// the worldspace has no large references (a pack of format 10 or older).
    godot::Error setup(const godot::Ref<SkydotPack>& pack, const godot::Ref<SkydotWorld>& world,
                       std::int64_t world_id, SkydotLod* lod);

    /// Cells around the camera whose large references are drawn.
    void set_radius(std::int64_t cells);
    std::int64_t get_radius() const { return radius_; }

    /// Full-detail cell (x, y) is built (or dropped): it draws its own large
    /// references, so this layer does not.
    void set_cell_built(std::int64_t x, std::int64_t y, bool built);

    /// Choose, load and build for a camera at `camera` (Godot space), within
    /// `budget_usec`. Returns how many wanted large references are not built yet.
    std::int64_t update(const godot::Vector3& camera, std::int64_t budget_usec);

    /// Drop every model, forget the built cells, and tell the LOD it no
    /// longer hides anything.
    void clear();

    /// wanted, shown, pending, candidates (drawable among those chosen),
    /// skipped (by reason: "disabled", "no base", "no model", "marker",
    /// "failed"), marked_cells, selections, and the slowest select_usec,
    /// drop_usec, build_usec and marks_usec of an update.
    godot::Dictionary get_stats() const;

protected:
    static void _bind_methods();

private:
    /// What a large reference draws, found once, and where it is in the
    /// layer's lifecycle.
    struct Info {
        std::string model; ///< Asset cache key; empty if it draws nothing.
        const char* reason = nullptr;
        std::int32_t cell_x = 0; ///< The square it stands in.
        std::int32_t cell_y = 0;
        double x = 0.0; ///< Game units.
        double y = 0.0;
        godot::Node3D* node = nullptr; ///< Built, once it is.
        std::uint64_t wanted = 0;      ///< The selection (`generation_`) that chose it.
    };
    Info& info(std::uint32_t index);
    /// The infos of the references listed for square (x, y), found once.
    const std::vector<Info*>& list(std::int32_t x, std::int32_t y);
    void select(std::int32_t cx, std::int32_t cy, double x, double y);
    void update_marks(std::int32_t cx, std::int32_t cy);
    bool built(std::int32_t x, std::int32_t y) const;
    /// Nothing more to draw for `i` here: built, not drawable, or its own cell draws it.
    bool settled(const Info& i) const { return i.node != nullptr || i.model.empty() || built(i.cell_x, i.cell_y); }
    static void no_shadows(godot::Node* node);
    static std::uint64_t key(std::int32_t x, std::int32_t y) {
        return static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32 | static_cast<std::uint32_t>(y);
    }

    godot::Ref<SkydotPack> pack_;
    godot::Ref<SkydotWorld> world_;
    SkydotLod* lod_ = nullptr;
    std::uint32_t world_id_ = 0;
    std::int64_t radius_ = 5;

    std::unordered_set<std::uint64_t> built_cells_;
    bool dirty_ = true; ///< The choice must be made again (camera moved a cell, a cell built).
    bool marks_dirty_ = true;
    std::int32_t cell_x_ = 0;
    std::int32_t cell_y_ = 0;
    bool placed_ = false;
    std::uint64_t generation_ = 0;

    /// By reference index; node-based, so pointers to the values stay valid.
    std::unordered_map<std::uint32_t, Info> infos_;
    std::unordered_map<std::uint64_t, std::vector<Info*>> lists_;
    /// Chosen drawable references, nearest the camera (when chosen) first.
    std::vector<std::pair<std::uint32_t, Info*>> candidates_;
    std::unordered_map<std::uint32_t, Info*> shown_;
    /// Models held, by asset cache key (null once failed).
    std::unordered_map<std::string, godot::Ref<godot::Resource>> models_;
    /// Cells marked on the LOD.
    std::set<std::pair<std::int32_t, std::int32_t>> marked_;

    std::int64_t selections_ = 0;
    /// The slowest of each part of an update, in microseconds.
    std::int64_t select_usec_ = 0;
    std::int64_t drop_usec_ = 0;
    std::int64_t build_usec_ = 0;
    std::int64_t marks_usec_ = 0;
    std::int64_t select_total_ = 0;
    std::int64_t marks_total_ = 0;
    std::int64_t marks_runs_ = 0;
    std::int64_t pending_ = 0;
    std::unordered_map<std::string, std::int64_t> skipped_;
};

} // namespace skydot
