// SPDX-License-Identifier: GPL-3.0-or-later
//
// `ActorPlacement`: where placed actors (ACHR) are now, when not where the
// editor put them. SkydotAi moves actors by setting places here; cells built
// afterwards build an actor where its place is. The editor's own places come
// from WorldData.
#pragma once

#include "data/world_data.hpp"

#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace skydot {

class ActorPlacement {
public:
    /// A space (an interior cell, or a worldspace for the outside), a position
    /// (Skyrim space) and a facing about Z.
    struct Place {
        std::uint32_t space{};
        godot::Vector3 position;
        float rotation_z{};
    };
    /// A placed actor and, if it was moved, where it is now.
    struct ActorAt {
        const bethconv::pack::wfb::ActorRef* actor{};
        const Place* place{};
    };
    /// Where an actor is: `place`, whether `space` is an interior, and the cell
    /// (the interior, or the exterior cell at the position; 0 if none).
    struct Where {
        Place place;
        bool interior{};
        std::uint32_t cell{};
        bool moved{}; ///< Whether `place` is not the editor's.
    };

    explicit ActorPlacement(std::shared_ptr<const WorldData> data) : data_(std::move(data)) {}

    /// Move `ref` to a place. A place a build under way points to stays valid
    /// until it is replaced or cleared.
    void set_place(std::uint32_t ref, std::uint32_t space, const godot::Vector3& position,
                   float rotation_z);
    /// Put `ref` back where the editor placed it; `clear_places` does so for
    /// every actor.
    void clear_place(std::uint32_t ref);
    void clear_places();
    /// Where `ref` was moved to; null if it stands where the editor put it.
    const Place* moved_place(std::uint32_t ref) const;
    /// Empty if `ref` is no placed actor.
    std::optional<Where> where(std::uint32_t ref) const;

    /// The actors that are in an interior cell (`cell`) or an exterior grid
    /// square now: those placed there that were not moved elsewhere, and
    /// those moved there.
    std::vector<ActorAt> actors_in_cell(std::uint32_t cell) const;
    std::vector<ActorAt> actors_in_grid(std::uint32_t world, std::int32_t x, std::int32_t y) const;

private:
    /// The bucket a place falls in: an interior cell id, or a grid key.
    std::uint64_t place_bucket(const Place& place) const;
    std::uint64_t placed_bucket(const bethconv::pack::wfb::ActorRef& actor) const;

    std::shared_ptr<const WorldData> data_;
    std::unordered_map<std::uint32_t, Place> places_;
    /// Bucket -> actors moved into it.
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> moved_in_;
};

} // namespace skydot
