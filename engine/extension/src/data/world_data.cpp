// SPDX-License-Identifier: GPL-3.0-or-later
#include "data/world_data.hpp"
#include "data/fb_search.hpp"

#include "skydot_formats/flags.hpp"
#include "world_generated.h"

#include <algorithm>
#include <array>
#include <utility>

namespace wfb = bethconv::pack::wfb;

namespace skydot {

namespace {

/// Sort `entries` (`ref << 32 | cell`) by ref, equal refs staying in order: a
/// least-significant-digit radix sort over the ref's 32 bits, 11 at a time:
/// about three times faster than std::sort on the references of a full game.
void sort_by_ref(std::vector<std::uint64_t>& entries) {
    constexpr unsigned k_digit_bits = 11;
    constexpr std::size_t k_digits = 3;
    constexpr std::size_t k_buckets = std::size_t{1} << k_digit_bits;
    const auto digit = [](std::uint64_t entry, std::size_t pass) {
        return static_cast<std::size_t>(entry >> (32 + pass * k_digit_bits)) & (k_buckets - 1);
    };
    std::array<std::array<std::size_t, k_buckets>, k_digits> counts{};
    for (const std::uint64_t entry : entries) {
        for (std::size_t pass = 0; pass < k_digits; ++pass) {
            ++counts[pass][digit(entry, pass)];
        }
    }
    std::vector<std::uint64_t> moved(entries.size());
    for (std::size_t pass = 0; pass < k_digits; ++pass) {
        auto& offsets = counts[pass];
        if (offsets[digit(entries.front(), pass)] == entries.size()) {
            continue; // every ref has the same digit here
        }
        std::size_t next = 0;
        for (std::size_t& offset : offsets) {
            next += std::exchange(offset, next);
        }
        for (const std::uint64_t entry : entries) {
            moved[offsets[digit(entry, pass)]++] = entry;
        }
        entries.swap(moved);
    }
}

} // namespace

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
    std::size_t ref_count = 0;
    for (const auto* cell : *cells) {
        ref_count += cell->refs() != nullptr ? cell->refs()->size() : 0;
    }
    ref_cells_.reserve(ref_count);
    for (const auto* cell : *cells) {
        if (const auto* navmeshes = cell->navmeshes()) {
            for (const auto* nav : *navmeshes) {
                navmeshes_.emplace(nav->id(), std::pair{nav, cell});
            }
        }
        if (const auto* refs = cell->refs()) {
            for (const auto* ref : *refs) {
                ref_cells_.push_back(static_cast<std::uint64_t>(ref->id()) << 32 | cell->id());
                if (ref->enable_parent() != 0) {
                    enable_children_.emplace(ref->enable_parent(), ref->id());
                }
            }
        }
    }
    if (!ref_cells_.empty()) {
        sort_by_ref(ref_cells_);
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

std::int64_t WorldData::cell_of_ref(std::int64_t ref) const {
    const auto id = static_cast<std::uint64_t>(static_cast<std::uint32_t>(ref));
    const auto it = std::ranges::lower_bound(ref_cells_, id << 32);
    return it != ref_cells_.end() && *it >> 32 == id ? static_cast<std::int64_t>(*it & 0xFFFFFFFFU) : 0;
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

bool WorldData::initially_disabled(const wfb::LargeRef& ref) const {
    // As for a placed reference: the enable parent's state, inverted if
    // flagged, found through the cell holding the parent.
    bool opposite = formats::has_flag(ref.flags(), wfb::RefFlags::enable_opposite);
    std::uint32_t parent = ref.enable_parent();
    if (parent == 0) {
        return formats::has_flag(ref.flags(), wfb::RefFlags::initially_disabled);
    }
    const wfb::Ref* r = nullptr;
    for (int depth = 0; depth < 16 && parent != 0; ++depth) {
        const auto* cell = cell_ptr(cell_of_ref(parent));
        const wfb::Ref* found = cell != nullptr ? lookup(cell->refs(), parent) : nullptr;
        if (found == nullptr) {
            break; // an unknown parent: the last known one decides
        }
        r = found;
        parent = r->enable_parent();
        if (parent != 0) {
            opposite ^= formats::has_flag(r->flags(), wfb::RefFlags::enable_opposite);
        }
    }
    if (r == nullptr) {
        return formats::has_flag(ref.flags(), wfb::RefFlags::initially_disabled);
    }
    return formats::has_flag(r->flags(), wfb::RefFlags::initially_disabled) != opposite;
}

std::uint32_t WorldData::large_ref_count(std::uint32_t world) const {
    const auto* w = world_ptr(world);
    return w != nullptr && w->large_refs() != nullptr ? w->large_refs()->size() : 0;
}

const wfb::LargeRef* WorldData::large_ref(std::uint32_t world, std::uint32_t index) const {
    const auto* w = world_ptr(world);
    const auto* refs = w != nullptr ? w->large_refs() : nullptr;
    return refs != nullptr && index < refs->size() ? refs->Get(index) : nullptr;
}

void WorldData::large_cell_refs(std::uint32_t world, std::int32_t x, std::int32_t y,
                                std::vector<std::uint32_t>& out) const {
    const auto* w = world_ptr(world);
    const auto* cells = w != nullptr ? w->large_cells() : nullptr;
    const auto* list = w != nullptr ? w->large_cell_refs() : nullptr;
    if (cells == nullptr || list == nullptr) {
        return;
    }
    // Sorted by (cell_y, cell_x).
    flatbuffers::uoffset_t lo = 0;
    flatbuffers::uoffset_t hi = cells->size();
    while (lo < hi) {
        const flatbuffers::uoffset_t mid = lo + (hi - lo) / 2;
        const auto* c = cells->Get(mid);
        if (std::pair{static_cast<std::int32_t>(c->cell_y()), static_cast<std::int32_t>(c->cell_x())} <
            std::pair{y, x}) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo >= cells->size()) {
        return;
    }
    const auto* c = cells->Get(lo);
    if (c->cell_x() != x || c->cell_y() != y) {
        return;
    }
    const std::uint64_t end = std::min<std::uint64_t>(std::uint64_t{c->first()} + c->count(), list->size());
    for (std::uint64_t i = c->first(); i < end; ++i) {
        out.push_back(list->Get(static_cast<flatbuffers::uoffset_t>(i)));
    }
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
