// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/actor_placement.hpp"

#include "skydot_formats/flags.hpp"
#include "world_generated.h"

#include <algorithm>

namespace wfb = bethconv::pack::wfb;

namespace skydot {

std::uint64_t ActorPlacement::place_bucket(const Place& place) const {
    const auto* cell = data_->cell_ptr(place.space);
    if (cell != nullptr && formats::has_flag(cell->flags(), wfb::CellFlags::interior)) {
        return place.space; // An interior: its id (grid keys have a world above bit 32).
    }
    return grid_key(place.space, grid_square(static_cast<float>(place.position.x)),
                    grid_square(static_cast<float>(place.position.y)));
}

std::uint64_t ActorPlacement::placed_bucket(const wfb::ActorRef& actor) const {
    const auto* cell = data_->cell_ptr(actor.cell());
    if (cell == nullptr) {
        return 0;
    }
    if (formats::has_flag(cell->flags(), wfb::CellFlags::interior) || cell->world() == 0) {
        return cell->id();
    }
    return grid_key(cell->world(), grid_square(actor.position().x()), grid_square(actor.position().y()));
}

std::vector<ActorPlacement::ActorAt> ActorPlacement::actors_in_cell(std::uint32_t cell) const {
    std::vector<ActorAt> out;
    const auto bucket = static_cast<std::uint64_t>(cell);
    const auto stays = [&](const wfb::ActorRef* a) {
        const auto it = places_.find(a->ref());
        return it == places_.end() || place_bucket(it->second) == bucket;
    };
    if (const auto* placed = data_->cell_actors(cell)) {
        for (const auto* a : *placed) {
            // An exterior cell's own actors are bucketed by its grid square;
            // interiors by the cell.
            if (places_.contains(a->ref())) {
                if (placed_bucket(*a) == bucket && stays(a)) {
                    out.push_back({a, &places_.at(a->ref())});
                }
                continue;
            }
            out.push_back({a, nullptr});
        }
    }
    if (const auto it = moved_in_.find(bucket); it != moved_in_.end()) {
        for (const auto ref : it->second) {
            const auto* a = data_->actor_ptr(ref);
            if (a != nullptr && placed_bucket(*a) != bucket) {
                out.push_back({a, &places_.at(ref)});
            }
        }
    }
    return out;
}

std::vector<ActorPlacement::ActorAt> ActorPlacement::actors_in_grid(std::uint32_t world, std::int32_t x,
                                                                    std::int32_t y) const {
    std::vector<ActorAt> out;
    const auto bucket = grid_key(world, x, y);
    const auto add_placed = [&](const std::vector<const wfb::ActorRef*>& list) {
        for (const auto* a : list) {
            const auto it = places_.find(a->ref());
            if (it == places_.end()) {
                out.push_back({a, nullptr});
            } else if (place_bucket(it->second) == bucket) {
                out.push_back({a, &it->second});
            }
        }
    };
    if (const auto* cell = data_->exterior_ptr(world, x, y)) {
        if (const auto* placed = data_->cell_actors(cell->id())) {
            add_placed(*placed);
        }
    }
    if (const auto* placed = data_->persistent_actors(world, x, y)) {
        add_placed(*placed);
    }
    if (const auto it = moved_in_.find(bucket); it != moved_in_.end()) {
        for (const auto ref : it->second) {
            const auto* a = data_->actor_ptr(ref);
            if (a != nullptr && placed_bucket(*a) != bucket) {
                out.push_back({a, &places_.at(ref)});
            }
        }
    }
    return out;
}

void ActorPlacement::set_place(std::uint32_t ref, std::uint32_t space, const godot::Vector3& position,
                               float rotation_z) {
    clear_place(ref);
    const Place place{space, position, rotation_z};
    places_[ref] = place;
    moved_in_[place_bucket(place)].push_back(ref);
}

void ActorPlacement::clear_place(std::uint32_t ref) {
    const auto it = places_.find(ref);
    if (it == places_.end()) {
        return;
    }
    if (const auto in = moved_in_.find(place_bucket(it->second)); in != moved_in_.end()) {
        std::erase(in->second, ref);
    }
    places_.erase(it);
}

void ActorPlacement::clear_places() {
    places_.clear();
    moved_in_.clear();
}

const ActorPlacement::Place* ActorPlacement::moved_place(std::uint32_t ref) const {
    const auto it = places_.find(ref);
    return it != places_.end() ? &it->second : nullptr;
}

std::optional<ActorPlacement::Where> ActorPlacement::where(std::uint32_t ref) const {
    const auto* a = data_->actor_ptr(ref);
    if (a == nullptr) {
        return std::nullopt;
    }
    Where out;
    out.place = {data_->cell_space(a->cell()),
                 godot::Vector3(a->position().x(), a->position().y(), a->position().z()),
                 a->rotation().z()};
    if (const auto* moved = moved_place(a->ref())) {
        out.place = *moved;
        out.moved = true;
    }
    const auto* space = data_->cell_ptr(out.place.space);
    out.interior = space != nullptr && formats::has_flag(space->flags(), wfb::CellFlags::interior);
    if (out.interior) {
        out.cell = out.place.space;
    } else if (const auto* c = data_->exterior_ptr(out.place.space,
                                                   grid_square(static_cast<float>(out.place.position.x)),
                                                   grid_square(static_cast<float>(out.place.position.y)))) {
        out.cell = c->id();
    }
    return out;
}

} // namespace skydot
