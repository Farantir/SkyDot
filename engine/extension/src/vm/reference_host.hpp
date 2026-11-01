// SPDX-License-Identifier: GPL-3.0-or-later
//
// `ReferenceHost`: what the ObjectReference natives change beyond what a
// world question or an event can say: the reference states a save carries,
// enable and open and lock changes with their side effects, moving, and the
// inventory of an owner.
#pragma once

#include "state/inventory.hpp"
#include "state/ref_states.hpp"
#include "vm/value.hpp"
#include "vm/world_access.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace skydot::vm {

/// How long PlayAnimationAndWait waits for the animation's event, in seconds.
inline constexpr double k_animation_timeout = 10.0;

class ReferenceHost {
public:
    virtual ~ReferenceHost() = default;

    /// The states scripts changed per reference (moved, disabled, blocked).
    [[nodiscard]] virtual RefStates& ref_states() = 0;
    /// Enable or disable `ref`, with what that does to its model and its scripts.
    virtual void set_disabled(std::uint32_t ref, bool disabled) = 0;
    /// `ref` is whole again: forget what destroyed it.
    virtual void clear_destruction(std::uint32_t ref) = 0;
    /// GetOpenState of `ref` (1 open, 3 closed).
    [[nodiscard]] virtual std::int64_t open_state(std::int64_t ref) const = 0;
    /// Set the open state of `ref`.
    virtual void set_open_state(std::int64_t ref, std::int64_t state) = 0;
    /// Lock or unlock `ref`.
    virtual void set_locked(std::int64_t ref, bool locked) = 0;
    /// Whether `ref` is locked.
    [[nodiscard]] virtual bool is_locked(std::uint32_t ref) const = 0;
    /// The lock level of `ref`; negative if it has none.
    [[nodiscard]] virtual std::int64_t lock_level(std::int64_t ref) const = 0;
    /// The key of the lock `ref` has (from the data); 0 if it needs none or has no lock.
    [[nodiscard]] virtual std::uint32_t lock_key(std::uint32_t ref) const = 0;
    /// How many references are inside the trigger `ref`.
    [[nodiscard]] virtual std::int64_t trigger_count(std::int64_t ref) const = 0;
    /// MoveTo: `ref` to `target`'s place plus `offset` in world axes, with its
    /// rotation if `match_rotation`.
    virtual void move_ref_to(std::uint32_t ref, std::uint32_t target, const Xyz& offset, bool match_rotation) = 0;
    /// TranslateTo: put `ref` at `position` with `rotation` (radians) at once.
    virtual void relocate_ref(std::uint32_t ref, const Xyz& position, const Xyz& rotation) = 0;
    /// Send `event` to `ref`'s scripts and to the aliases it fills.
    virtual void send_event(std::uint32_t ref, std::string_view event, const std::vector<Value>& args) = 0;
    /// Give `owner` its start inventory if it has not got it yet.
    virtual void resolve_owner(std::uint32_t owner) const = 0;
    /// Take `count` of `form` from `owner`: plain units first, extras with their stack.
    virtual std::vector<Inventory::Taken> remove_item_split(std::uint32_t owner, std::uint32_t form,
                                                            std::int64_t count) = 0;
};

} // namespace skydot::vm
