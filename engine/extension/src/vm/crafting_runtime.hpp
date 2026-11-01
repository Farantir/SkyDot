// SPDX-License-Identifier: GPL-3.0-or-later
//
// The crafting domain of the Papyrus runtime: owns the crafting rules
// (vm/crafting.hpp) over the VM's inventory and hosts them (benches, keywords,
// the player's conditions). No Godot: it reaches the engine through
// CraftingRuntime::Host and WorldAccess.
#pragma once

#include "conditions/conditions.hpp"
#include "state/created_forms.hpp"
#include "state/inventory.hpp"
#include "state/item_extras.hpp"
#include "vm/crafting.hpp"
#include "vm/world_access.hpp"
#include "world_generated.h"

#include <memory>
#include <string>

namespace skydot::vm {

class CraftingRuntime final : public crafting::CraftingHost {
public:
    class Host {
    public:
        virtual ~Host() = default;
        /// The pack's world records; null without a world.
        [[nodiscard]] virtual const bethconv::pack::wfb::World* world_root() const = 0;
        [[nodiscard]] virtual WorldAccess& world_access() = 0;
        /// The VM's inventory (it changes with each `setup`, so the runtime is reset then).
        [[nodiscard]] virtual Inventory& inventory() = 0;
        [[nodiscard]] virtual ItemExtras& item_extras() = 0;
        [[nodiscard]] virtual CreatedForms& created_forms() = 0;
        /// A condition world of its own for one evaluation (the player's conditions of a recipe).
        [[nodiscard]] virtual std::unique_ptr<conditions::ConditionWorld> new_condition_world() = 0;
        /// A line for the player and the log.
        virtual void announce(const std::string& line) = 0;
    };

    /// Remember the host (SkydotPapyrus::bind_natives).
    void bind(Host& host) { host_ = &host; }
    /// Drop the rules (they refer to the old VM's inventory).
    void reset() { crafting_.reset(); }
    /// The crafting rules, made on first use; null without a world or a host.
    crafting::Crafting* logic();

    // crafting::CraftingHost
    bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<bethconv::pack::wfb::Condition>>* list,
                         const crafting::ConditionItem* item) override;
    int bench_type(std::uint32_t bench_ref) const override;
    bool bench_has_keyword(std::uint32_t bench_ref, std::uint32_t keyword) const override;
    std::uint32_t keyword(std::string_view editor_id) const override;
    void log(const std::string& line) override;

private:
    Host* host_ = nullptr;
    std::unique_ptr<crafting::Crafting> crafting_;
};

} // namespace skydot::vm
