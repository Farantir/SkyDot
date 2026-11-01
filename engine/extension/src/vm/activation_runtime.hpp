// SPDX-License-Identifier: GPL-3.0-or-later
//
// The activation domain of the Papyrus runtime: owns the rules and sequence of
// activations (vm/activation.hpp) and the state of harvested plants, and answers
// what they ask of the world (targets, locks, children, pickups, harvests). No
// Godot: it reaches the engine through ActivationRuntime::Host and WorldAccess.
#pragma once

#include "state/actor_states.hpp"
#include "state/inventory.hpp"
#include "vm/activation.hpp"
#include "vm/harvest.hpp"
#include "vm/item_access.hpp"
#include "vm/reference_host.hpp"
#include "vm/world_access.hpp"
#include "world_generated.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skydot::vm {

class ActivationRuntime final : public ActivationHost {
public:
    class Host {
    public:
        virtual ~Host() = default;
        /// The pack's world records (harvest data, bench types, leveled lists); null without a world.
        [[nodiscard]] virtual const bethconv::pack::wfb::World* world_root() const = 0;
        /// ref_info, activate_children, base_form and game_days.
        [[nodiscard]] virtual WorldAccess& world_access() = 0;
        /// Blocked activations, enabling, locks and start inventories.
        [[nodiscard]] virtual ReferenceHost& references() = 0;
        [[nodiscard]] virtual Inventory& inventory() = 0;
        /// Who is dead (a dead actor opens its inventory when activated).
        [[nodiscard]] virtual ActorStates& actor_states() = 0;
        /// The NPC_ of a placed actor the data has no cell for; 0 if `ref` is none.
        [[nodiscard]] virtual std::uint32_t placed_actor_base(std::uint32_t ref) const = 0;
        /// Whether the reference's behaviour graph raises ObjectActivated.
        [[nodiscard]] virtual bool graph_raises_activation(std::uint32_t ref) const = 0;
        /// OnActivate(activator) to `ref`'s scripts; how many handlers ran.
        virtual std::int64_t send_activate(std::uint32_t ref, std::int64_t activator) = 0;
        /// Run an activation of `child` by `parent` (ActivationSource::parent) after `delay` VM seconds.
        virtual void schedule_activation(double delay, std::uint32_t child, std::uint32_t parent) = 0;
        /// Start inventories, item names and dropped items.
        [[nodiscard]] virtual ItemAccess& items() = 0;
        /// The value of GLOB `id`; nullopt if the world has no such global.
        [[nodiscard]] virtual std::optional<double> global_value_of(std::uint32_t id) const = 0;
        /// Forget everything the VM keeps of `ref` (it is gone).
        virtual void forget_ref(std::uint32_t ref) = 0;
    };

    /// Remember the host; the host outlives the runtime's use.
    void bind(Host& host) { host_ = &host; }

    /// See Activations::activate and ::complete.
    ActivationOutcome activate(std::uint32_t ref, std::int64_t activator, bool default_only, ActivationSource source,
                               bool force);
    std::optional<ActivationOutcome> complete(std::uint32_t ref);

    [[nodiscard]] bool pending(std::uint32_t ref) const { return activations_.pending(ref); }
    void forget(std::uint32_t ref) { activations_.forget(ref); }
    void held_refs(std::set<std::uint32_t>& out) const { activations_.held_refs(out); }
    [[nodiscard]] std::size_t pending_count() const { return activations_.pending_count(); }
    /// Drop the pending activations and the harvest state (a new game).
    void reset() {
        activations_.clear();
        harvests_.clear();
    }

    HarvestStates& harvests() { return harvests_; }
    [[nodiscard]] const HarvestStates& harvests() const { return harvests_; }
    /// The HRVS section's bytes.
    [[nodiscard]] std::vector<std::uint8_t> save() const { return harvests_.save(); }
    void adopt(HarvestStates&& loaded) { harvests_.adopt(std::move(loaded)); }

    // ActivationHost
    bool target(std::uint32_t ref, ActivationTarget& out) const override;
    int lock_level(std::uint32_t ref) const override;
    void unlock(std::uint32_t ref) override;
    bool graph_raises_activation(std::uint32_t ref) const override;
    std::int64_t send_activate(std::uint32_t ref, std::int64_t activator) override;
    bool activation_blocked(std::uint32_t ref) const override;
    std::vector<Child> activate_children(std::uint32_t ref) const override;
    void schedule_child(double delay, std::uint32_t child, std::uint32_t parent) override;
    void pick_up(std::uint32_t ref, std::int64_t activator, const ActivationTarget& item) override;
    bool harvested(std::uint32_t ref) const override;
    HarvestResult harvest(std::uint32_t ref, std::int64_t activator) override;

private:
    [[nodiscard]] const bethconv::pack::wfb::Base* base_of(std::uint32_t base) const;

    Host* host_ = nullptr;
    HarvestStates harvests_;
    Activations activations_{*this};
};

} // namespace skydot::vm
