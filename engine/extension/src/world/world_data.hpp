// SPDX-License-Identifier: GPL-3.0-or-later
//
// `WorldData`: a pack's `world.fb` (formats/pack-format.md, schema
// formats/schema/world.fbs) as the engine reads it: the verified bytes and the
// indexes built over them once, in `open`. Nothing changes afterwards, so one
// can be shared by everything that reads the world
// (`std::shared_ptr<const WorldData>`), from any thread. A closed one answers
// every query with nothing.
#pragma once

#include "world/navmesh.hpp"
#include "skydot_formats/units.hpp"

#include <godot_cpp/variant/packed_byte_array.hpp>

#include <cmath>
#include <cstdint>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace bethconv::pack::wfb {
struct World;
struct Cell;
struct Base;
struct Ref;
struct ActorRef;
struct Worldspace;
struct Water;
struct DoorLink;
} // namespace bethconv::pack::wfb

namespace skydot {

/// Key of exterior grid square (x, y) of a worldspace: the worldspace above
/// bit 32, then the square's coordinates as 16 bits each.
inline std::uint64_t grid_key(std::uint32_t world, std::int32_t x, std::int32_t y) {
    return (static_cast<std::uint64_t>(world) << 32) |
           (static_cast<std::uint64_t>(static_cast<std::uint16_t>(x)) << 16) |
           static_cast<std::uint64_t>(static_cast<std::uint16_t>(y));
}

/// The grid square a coordinate in game units falls in.
inline std::int32_t grid_square(float units) {
    return static_cast<std::int32_t>(std::floor(units / static_cast<float>(formats::k_cell_units)));
}

class WorldData {
public:
    /// The world.fb format version this engine writes against. Format 8
    /// (before AI packages) still reads; its actors have no packages.
    static constexpr int FORMAT_VERSION = 10;
    static constexpr int FORMAT_VERSION_MIN = 8;

    enum class Status {
        ok,
        corrupt,     ///< Empty, or the FlatBuffers verifier refused it.
        unsupported, ///< A format version outside FORMAT_VERSION_MIN..FORMAT_VERSION.
    };
    struct OpenResult {
        Status status{};
        int version{}; ///< The file's format version, if it verified.
    };

    WorldData() = default;
    WorldData(const WorldData&) = delete;
    WorldData& operator=(const WorldData&) = delete;

    /// Verify `bytes` and index them. A refused file leaves this closed and
    /// empty. Once only: every index points into the bytes this keeps, so
    /// open a new WorldData for another file.
    OpenResult open(godot::PackedByteArray bytes);
    bool is_open() const { return root_ != nullptr; }
    /// The verified root; null while closed.
    const bethconv::pack::wfb::World* root() const { return root_; }

    // ---- lookups: null when there is no such record ------------------------
    const bethconv::pack::wfb::Cell* cell_ptr(std::int64_t id) const;
    const bethconv::pack::wfb::Base* base_ptr(std::int64_t id) const;
    const bethconv::pack::wfb::Worldspace* world_ptr(std::int64_t id) const;
    const bethconv::pack::wfb::Water* water_ptr(std::uint32_t id) const;
    const bethconv::pack::wfb::ActorRef* actor_ptr(std::int64_t ref) const;
    /// Exterior cell (x, y) of `world`; persistent cells are not among them.
    const bethconv::pack::wfb::Cell* exterior_ptr(std::uint32_t world, std::int32_t x,
                                                  std::int32_t y) const;
    /// The worldspace whose terrain `world` shows: its parent when PNAM bit 0
    /// (use land data) is set.
    std::uint32_t land_world(std::uint32_t world) const;
    /// The water type of exterior cell (x, y): its XCWT, else the worldspace's.
    std::uint32_t water_type(std::uint32_t world, const bethconv::pack::wfb::Cell* cell) const;
    /// The load door `ref` as its link, or null.
    const bethconv::pack::wfb::DoorLink* door_ptr(std::uint32_t ref) const;

    // ---- indexes -----------------------------------------------------------
    /// Load door ref -> the cell holding it and its link.
    using DoorIndex = std::unordered_map<std::uint32_t, std::pair<const bethconv::pack::wfb::Cell*,
                                                                  const bethconv::pack::wfb::DoorLink*>>;
    const DoorIndex& doors() const { return doors_; }
    /// Every navmesh by id, with the cell holding it.
    const NavIndex& navmeshes() const { return navmeshes_; }
    /// Persistent references positioned in grid square (x, y) of `world`; null
    /// if none.
    const std::vector<const bethconv::pack::wfb::Ref*>* persistent_refs(std::uint32_t world,
                                                                        std::int32_t x,
                                                                        std::int32_t y) const;
    /// The persistent cell of `world`, or 0.
    std::uint32_t persistent_cell(std::uint32_t world) const;
    /// References activated when `ref` is: (child, cell, delay), sorted.
    using ActivateChild = std::tuple<std::uint32_t, std::uint32_t, float>;
    std::vector<ActivateChild> activate_children(std::uint32_t ref) const;
    /// References whose enable state follows `ref`'s, sorted.
    std::vector<std::uint32_t> enable_children(std::uint32_t ref) const;
    /// Initially disabled, following the enable parent chain.
    bool initially_disabled(const bethconv::pack::wfb::Ref& ref) const;
    /// Placed actors of an interior or exterior cell; null if none. Those of a
    /// worldspace's persistent cell are by the grid square they stand in.
    const std::vector<const bethconv::pack::wfb::ActorRef*>* cell_actors(std::uint32_t cell) const;
    const std::vector<const bethconv::pack::wfb::ActorRef*>* persistent_actors(std::uint32_t world,
                                                                               std::int32_t x,
                                                                               std::int32_t y) const;
    /// NPC_ -> its lowest placed actor, or 0.
    std::uint32_t actor_of(std::uint32_t npc) const;

private:
    void build_indexes();

    godot::PackedByteArray bytes_;
    const bethconv::pack::wfb::World* root_{};

    /// (world, x, y) -> exterior cell, persistent cells excluded.
    std::unordered_map<std::uint64_t, const bethconv::pack::wfb::Cell*> exteriors_;
    /// (world, x, y) -> persistent references positioned in that cell.
    std::unordered_map<std::uint64_t, std::vector<const bethconv::pack::wfb::Ref*>> persistent_;
    NavIndex navmeshes_;
    /// Worldspace -> its persistent cell.
    std::unordered_map<std::uint32_t, std::uint32_t> persistent_cells_;
    /// Activate parent -> (child, cell, delay).
    std::unordered_multimap<std::uint32_t, ActivateChild> activate_children_;
    /// Enable parent -> its enable children.
    std::unordered_multimap<std::uint32_t, std::uint32_t> enable_children_;
    /// Enable parents by id.
    std::unordered_map<std::uint32_t, const bethconv::pack::wfb::Ref*> enable_parents_;
    DoorIndex doors_;
    /// Placed actors by interior or exterior cell; those of a worldspace's
    /// persistent cell by the grid square they stand in.
    std::unordered_map<std::uint32_t, std::vector<const bethconv::pack::wfb::ActorRef*>> cell_actors_;
    std::unordered_map<std::uint64_t, std::vector<const bethconv::pack::wfb::ActorRef*>> persistent_actors_;
    std::unordered_map<std::uint32_t, std::uint32_t> actor_of_;
};

} // namespace skydot
