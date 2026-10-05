// SPDX-License-Identifier: GPL-3.0-or-later
#include "data/world_data.hpp"
#include "data/fb_search.hpp"

#include "skydot_formats/flags.hpp"
#include "world_generated.h"

#include <algorithm>

namespace wfb = bethconv::pack::wfb;

namespace skydot {

WorldData::OpenResult WorldData::open(const std::string& path) {
    auto map = std::make_unique<MappedFile>();
    std::string error;
    if (!map->open(path, error)) {
        return {Status::unreadable, 0, std::move(error)};
    }
    const auto bytes = map->bytes();
    // An empty file maps to no data at all; the root is read only if it has.
    flatbuffers::Verifier verifier(bytes.data(), bytes.size());
    if (bytes.empty() || !wfb::VerifyWorldBuffer(verifier)) {
        return {Status::corrupt, 0, {}};
    }
    const auto version = static_cast<int>(wfb::GetWorld(bytes.data())->format_version());
    if (version < FORMAT_VERSION_MIN || version > FORMAT_VERSION) {
        return {Status::unsupported, version, {}};
    }
    map_ = std::move(map);
    root_ = wfb::GetWorld(bytes.data());
    build_indexes();
    return {Status::ok, version, {}};
}

void WorldData::build_indexes() {
    if (const auto* actors = root_->actors()) {
        // Sorted by ref, so the first seen is the lowest.
        for (const auto* a : *actors) {
            actor_of_.try_emplace(a->base(), a->ref());
        }
    }
    const auto* cells = root_->cells();
    if (cells == nullptr) {
        return;
    }
    if (const auto* actors = root_->actors()) {
        for (const auto* a : *actors) {
            const auto* cell = cell_ptr(a->cell());
            if (cell == nullptr) {
                continue;
            }
            if (cell->persistent() && cell->world() != 0 &&
                !formats::has_flag(cell->flags(), wfb::CellFlags::interior)) {
                persistent_actors_[grid_key(cell->world(), grid_square(a->position().x()),
                                            grid_square(a->position().y()))]
                    .push_back(a);
            } else {
                cell_actors_[cell->id()].push_back(a);
            }
        }
    }
    for (const auto* cell : *cells) {
        if (const auto* navmeshes = cell->navmeshes()) {
            for (const auto* nav : *navmeshes) {
                navmeshes_.emplace(nav->id(), std::pair{nav, cell});
            }
        }
        if (const auto* refs = cell->refs()) {
            for (const auto* ref : *refs) {
                if (ref->enable_parent() != 0) {
                    enable_children_.emplace(ref->enable_parent(), ref->id());
                }
            }
        }
    }
    for (const auto* cell : *cells) {
        if (const auto* refs = cell->refs()) {
            for (const auto* ref : *refs) {
                if (enable_children_.contains(ref->id())) {
                    enable_parents_.emplace(ref->id(), ref);
                }
            }
        }
        if (const auto* doors = cell->doors()) {
            for (const auto* door : *doors) {
                doors_.emplace(door->ref(), std::pair{cell, door});
            }
        }
        if (const auto* parents = cell->activate_parents()) {
            for (const auto* p : *parents) {
                activate_children_.emplace(p->parent(), ActivateChild{p->ref(), cell->id(), p->delay()});
            }
        }
        if (formats::has_flag(cell->flags(), wfb::CellFlags::interior) || cell->world() == 0) {
            continue;
        }
        if (cell->persistent()) {
            persistent_cells_.emplace(cell->world(), cell->id());
            if (const auto* refs = cell->refs()) {
                for (const auto* ref : *refs) {
                    persistent_[grid_key(cell->world(), grid_square(ref->position().x()),
                                         grid_square(ref->position().y()))]
                        .push_back(ref);
                }
            }
        } else if (cell->has_grid()) {
            exteriors_.emplace(grid_key(cell->world(), cell->grid_x(), cell->grid_y()), cell);
        }
    }
}

// ---- lookups --------------------------------------------------------------

const wfb::Cell* WorldData::cell_ptr(std::int64_t id) const {
    const auto* cells = root_ != nullptr ? root_->cells() : nullptr;
    if (cells == nullptr) {
        return nullptr;
    }
    return lookup(cells, static_cast<std::uint32_t>(id));
}

const wfb::Base* WorldData::base_ptr(std::int64_t id) const {
    const auto* bases = root_ != nullptr ? root_->bases() : nullptr;
    if (bases == nullptr) {
        return nullptr;
    }
    return lookup(bases, static_cast<std::uint32_t>(id));
}

const wfb::Worldspace* WorldData::world_ptr(std::int64_t id) const {
    const auto* worlds = root_ != nullptr ? root_->worlds() : nullptr;
    if (worlds == nullptr) {
        return nullptr;
    }
    return lookup(worlds, static_cast<std::uint32_t>(id));
}

const wfb::Water* WorldData::water_ptr(std::uint32_t id) const {
    const auto* waters = root_ != nullptr ? root_->waters() : nullptr;
    if (waters == nullptr || id == 0) {
        return nullptr;
    }
    return lookup(waters, id);
}

const wfb::ActorRef* WorldData::actor_ptr(std::int64_t ref) const {
    return root_ != nullptr ? lookup(root_->actors(), static_cast<std::uint32_t>(ref)) : nullptr;
}

const wfb::Cell* WorldData::exterior_ptr(std::uint32_t world, std::int32_t x, std::int32_t y) const {
    const auto it = exteriors_.find(grid_key(world, x, y));
    return it != exteriors_.end() ? it->second : nullptr;
}

std::uint32_t WorldData::land_world(std::uint32_t world) const {
    const auto* w = world_ptr(world);
    if (w != nullptr && w->parent() != 0 &&
        formats::has_flag(w->parent_flags(), wfb::ParentFlags::land_data)) {
        return w->parent();
    }
    return world;
}

std::uint32_t WorldData::cell_space(std::int64_t id) const {
    const auto* cell = cell_ptr(id);
    if (cell == nullptr) {
        return 0;
    }
    return formats::has_flag(cell->flags(), wfb::CellFlags::interior) || cell->world() == 0 ? cell->id()
                                                                                          : cell->world();
}

std::uint32_t WorldData::water_type(std::uint32_t world, const wfb::Cell* cell) const {
    if (cell != nullptr && cell->water() != 0) {
        return cell->water();
    }
    const auto* w = world_ptr(world);
    return w != nullptr ? w->water() : 0;
}

const wfb::DoorLink* WorldData::door_ptr(std::uint32_t ref) const {
    const auto it = doors_.find(ref);
    return it != doors_.end() ? it->second.second : nullptr;
}

// ---- indexes --------------------------------------------------------------

const std::vector<const wfb::Ref*>* WorldData::persistent_refs(std::uint32_t world, std::int32_t x,
                                                               std::int32_t y) const {
    const auto it = persistent_.find(grid_key(world, x, y));
    return it != persistent_.end() ? &it->second : nullptr;
}

std::uint32_t WorldData::persistent_cell(std::uint32_t world) const {
    const auto it = persistent_cells_.find(world);
    return it != persistent_cells_.end() ? it->second : 0;
}

std::vector<WorldData::ActivateChild> WorldData::activate_children(std::uint32_t ref) const {
    const auto [begin, end] = activate_children_.equal_range(ref);
    std::vector<ActivateChild> out;
    for (auto it = begin; it != end; ++it) {
        out.push_back(it->second);
    }
    std::ranges::sort(out); // the multimap's order is unspecified
    return out;
}

std::vector<std::uint32_t> WorldData::enable_children(std::uint32_t ref) const {
    const auto [begin, end] = enable_children_.equal_range(ref);
    std::vector<std::uint32_t> out;
    for (auto it = begin; it != end; ++it) {
        out.push_back(it->second);
    }
    std::ranges::sort(out);
    return out;
}

bool WorldData::initially_disabled(const wfb::Ref& ref) const {
    // A reference with an enable parent takes the parent's state (inverted if
    // flagged); its own flag counts only when the parent is unknown.
    const wfb::Ref* r = &ref;
    bool opposite = false;
    for (int depth = 0; depth < 16 && r->enable_parent() != 0; ++depth) {
        const auto it = enable_parents_.find(r->enable_parent());
        if (it == enable_parents_.end()) {
            break;
        }
        opposite ^= formats::has_flag(r->flags(), wfb::RefFlags::enable_opposite);
        r = it->second;
    }
    return formats::has_flag(r->flags(), wfb::RefFlags::initially_disabled) != opposite;
}

const std::vector<const wfb::ActorRef*>* WorldData::cell_actors(std::uint32_t cell) const {
    const auto it = cell_actors_.find(cell);
    return it != cell_actors_.end() ? &it->second : nullptr;
}

const std::vector<const wfb::ActorRef*>* WorldData::persistent_actors(std::uint32_t world,
                                                                      std::int32_t x,
                                                                      std::int32_t y) const {
    const auto it = persistent_actors_.find(grid_key(world, x, y));
    return it != persistent_actors_.end() ? &it->second : nullptr;
}

std::uint32_t WorldData::actor_of(std::uint32_t npc) const {
    const auto it = actor_of_.find(npc);
    return it != actor_of_.end() ? it->second : 0;
}

} // namespace skydot
