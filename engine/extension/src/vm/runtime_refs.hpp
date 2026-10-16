// SPDX-License-Identifier: GPL-3.0-or-later
//
// What scripts make or change at run time and the VM keeps (and saves): the
// references PlaceAtMe made, and the damage destructible references took.
// Nothing here touches Godot.
#pragma once

#include <cstdint>
#include <map>

#include "vm/id_ranges.hpp"

namespace skydot::vm {

/// A reference made by PlaceAtMe: a runtime form id, its base, the cell it
/// was placed in and where (Skyrim space, game units; rotation in radians).
struct PlacedRef {
    std::uint32_t base = 0;
    std::uint32_t cell = 0;
    double position[3] = {0.0, 0.0, 0.0};
    double rotation[3] = {0.0, 0.0, 0.0};
    double scale = 1.0;
    /// An effect (an explosion): removes itself, and is not kept in saves.
    bool transient = false;
};

class PlacedRefs {
public:
    /// Runtime form ids come from id_ranges::runtime_refs (the game's,
    /// 0xFF......), above the ids `create_instance` gives test objects and
    /// below the alias handles.
    static constexpr std::uint32_t FIRST = id_ranges::runtime_refs.first;

    /// The new reference's id; 0 (and nothing added) once the range is used up.
    std::uint32_t add(const PlacedRef& ref) {
        const std::uint32_t id = ids_.allocate();
        if (id != 0) {
            refs_[id] = ref;
        }
        return id;
    }
    [[nodiscard]] const PlacedRef* find(std::uint32_t id) const {
        const auto it = refs_.find(id);
        return it != refs_.end() ? &it->second : nullptr;
    }
    bool remove(std::uint32_t id) { return refs_.erase(id) != 0; }
    void clear() {
        refs_.clear();
        ids_.clear();
    }
    [[nodiscard]] const std::map<std::uint32_t, PlacedRef>& refs() const { return refs_; }
    [[nodiscard]] std::uint32_t next() const { return ids_.next(); }
    /// False for an id outside the range.
    bool restore(std::uint32_t id, const PlacedRef& ref) {
        if (!id_ranges::runtime_refs.contains(id)) {
            return false;
        }
        refs_[id] = ref;
        ids_.note_used(id);
        return true;
    }
    /// Continue from a saved counter if it is above the one now; false if it
    /// lies outside the range.
    bool set_next(std::uint32_t next) {
        return next >= ids_.next() ? ids_.restore(next) : ids_.range().contains(next);
    }

private:
    std::map<std::uint32_t, PlacedRef> refs_;
    IdAllocator ids_{id_ranges::runtime_refs};
};

/// How much a destructible reference has taken. `stage` counts the
/// destruction stages opened (0: intact).
struct DestructionState {
    double damage = 0.0;
    std::int32_t stage = 0;
    bool destroyed = false;
};

class DestructionStates {
public:
    [[nodiscard]] DestructionState get(std::uint32_t ref) const {
        const auto it = states_.find(ref);
        return it != states_.end() ? it->second : DestructionState{};
    }
    void set(std::uint32_t ref, const DestructionState& state) { states_[ref] = state; }
    void erase(std::uint32_t ref) { states_.erase(ref); }
    void clear() { states_.clear(); }
    [[nodiscard]] const std::map<std::uint32_t, DestructionState>& states() const { return states_; }

private:
    std::map<std::uint32_t, DestructionState> states_;
};

} // namespace skydot::vm
