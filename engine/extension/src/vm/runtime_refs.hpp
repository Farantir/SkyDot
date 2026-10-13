// SPDX-License-Identifier: GPL-3.0-or-later
//
// What scripts make or change at run time and the VM keeps (and saves): the
// references PlaceAtMe made, and the damage destructible references took.
// Nothing here touches Godot.
#pragma once

#include <cstdint>
#include <map>

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
    /// Runtime form ids start here (the game's, 0xFF......), above the ids
    /// `create_instance` gives test objects and below the alias handles.
    static constexpr std::uint32_t FIRST = 0xFF001000u;

    std::uint32_t add(const PlacedRef& ref) {
        const std::uint32_t id = next_++;
        refs_[id] = ref;
        return id;
    }
    [[nodiscard]] const PlacedRef* find(std::uint32_t id) const {
        const auto it = refs_.find(id);
        return it != refs_.end() ? &it->second : nullptr;
    }
    bool remove(std::uint32_t id) { return refs_.erase(id) != 0; }
    void clear() {
        refs_.clear();
        next_ = FIRST;
    }
    [[nodiscard]] const std::map<std::uint32_t, PlacedRef>& refs() const { return refs_; }
    [[nodiscard]] std::uint32_t next() const { return next_; }
    void restore(std::uint32_t id, const PlacedRef& ref) {
        refs_[id] = ref;
        if (id >= next_) {
            next_ = id + 1;
        }
    }
    void set_next(std::uint32_t next) { next_ = next; }

private:
    std::map<std::uint32_t, PlacedRef> refs_;
    std::uint32_t next_ = FIRST;
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
