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
#include <utility>
#include <vector>

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
        /// The player's current value of actor value `av` (a skill's, fortify included).
        [[nodiscard]] virtual double player_actor_value(std::uint32_t av) = 0;
        /// `value` after the player's perks at entry point `ep`.
        [[nodiscard]] virtual double perk_entry(perks::EntryPoint ep, const perks::EntryContext& ctx, double value) = 0;
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
    double skill(int av) const override;
    double perk_entry(perks::EntryPoint ep, const perks::EntryContext& ctx, double value) const override;
    std::string gmst_text(std::string_view name, std::string_view fallback) const override;
    void skill_use(const crafting::SkillUse& use) override { uses_.push_back(use); }

    /// The skill use the crafts since the last call earned (Crafting's, and the enchanting table's through `record`).
    std::vector<crafting::SkillUse> take_skill_uses() { return std::exchange(uses_, {}); }
    void record(const crafting::SkillUse& use) { uses_.push_back(use); }

private:
    Host* host_ = nullptr;
    std::unique_ptr<crafting::Crafting> crafting_;
    std::vector<crafting::SkillUse> uses_;
};

} // namespace skydot::vm
