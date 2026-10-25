// SPDX-License-Identifier: GPL-3.0-or-later
//
// Crafting at benches, from the world's COBJ recipes and the player's items.
// The rules are pure; what only the game knows (conditions, the furniture's
// keywords and bench type, the log) comes through `CraftingHost`, as
// ActivationHost does for activations. Nothing here touches Godot.
//
// A bench is a FURN with a bench type (FURN WBDT):
//   1 create object (forge, smelter, tanning rack, cook pot): the COBJ whose
//     bench keyword the furniture carries.
//   2 smithing weapon (grindstone), 7 smithing armor (workbench): tempering,
//     the COBJ with the matching tempering keyword whose product is carried.
//   3 and 4 enchanting: not here; magic::Enchanting has the rules and
//     SkydotPapyrus (papyrus_enchanting.cpp) lists the table's rows.
//   5 and 6 alchemy: mixing 2 or 3 ingredients (see `preview`).
// A recipe is listed when its conditions pass (subject: the player; a
// function the evaluator does not know fails it), and craftable when the
// player holds every component.
#pragma once

#include "vm/created_forms.hpp"
#include "vm/inventory.hpp"
#include "vm/item_extras.hpp"

#include "world_generated.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace skydot::crafting {

namespace wfb = bethconv::pack::wfb;

namespace bench {
inline constexpr int create_object = 1;
inline constexpr int smithing_weapon = 2;
inline constexpr int enchanting = 3;
inline constexpr int enchanting_experiment = 4;
inline constexpr int alchemy = 5;
inline constexpr int alchemy_experiment = 6;
inline constexpr int smithing_armor = 7;
} // namespace bench

/// Keywords (editor ids) that pick the tempering recipes.
inline constexpr std::string_view k_weapon_tempering_keyword = "CraftingSmithingSharpeningWheel";
inline constexpr std::string_view k_armor_tempering_keyword = "CraftingSmithingArmorTable";

/// PLACEHOLDER (P6, skills and perks): the player's Smithing and Alchemy
/// skill is a constant until actor skills exist.
inline constexpr double k_placeholder_skill = 15.0;
/// PLACEHOLDER (P6): the tempering tier the skill reaches without perks:
/// skill 15 gives Fine.
inline constexpr float k_placeholder_temper_health = 1.1F;
/// Fallbacks of the GMSTs fAlchemyIngredientInitMult and fAlchemySkillFactor
/// (their values in Skyrim.esm), for a world without game settings.
inline constexpr double k_alchemy_init_mult = 4.0;
inline constexpr double k_alchemy_skill_factor = 1.5;
/// A created potion's weight.
inline constexpr float k_potion_weight = 0.5F;
/// MGEF flags.
inline constexpr std::uint32_t k_mgef_hostile = 0x1;
inline constexpr std::uint32_t k_mgef_power_affects_magnitude = 0x200000;
inline constexpr std::uint32_t k_mgef_power_affects_duration = 0x400000;

struct Component {
    std::uint32_t form = 0;
    std::int64_t need = 0;
    std::int64_t have = 0;
};

struct RecipeRow {
    std::uint32_t recipe = 0;
    std::uint32_t product = 0;
    std::int32_t count = 1;
    std::vector<Component> components;
    bool craftable = false;
};

struct TemperRow {
    std::uint32_t recipe = 0;
    std::uint32_t form = 0;
    std::uint32_t extra = 0;
    std::int64_t count = 0;
    float health = 1.0F;
    /// The health tempering would give; the current one if it would not improve.
    float next_health = 1.0F;
    std::vector<Component> components;
    /// Components held and the tempering would improve the item.
    bool craftable = false;
};

struct AlchemyEffect {
    std::uint32_t effect = 0;
    bool known = false;
};

struct AlchemyRow {
    std::uint32_t form = 0;
    std::int64_t count = 0;
    std::vector<AlchemyEffect> effects;
};

struct AlchemyPreview {
    std::string name;
    bool poison = false;
    std::int32_t value = 0;
    std::vector<vm::CreatedEffect> effects;
    /// 2 or 3 different ingredients that share an effect.
    bool mixable = false;
};

/// What the item of a temper recipe's conditions is asked about
/// (EPTemperingItemIsEnchanted).
struct ConditionItem {
    std::uint32_t form = 0;
    bool enchanted = false;
};

class CraftingHost {
public:
    virtual ~CraftingHost() = default;
    /// Whether `list` passes for the player; `item` is the item being tempered, if any.
    [[nodiscard]] virtual bool conditions_pass(
        const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list, const ConditionItem* item) = 0;
    [[nodiscard]] virtual int bench_type(std::uint32_t bench_ref) const = 0;
    [[nodiscard]] virtual bool bench_has_keyword(std::uint32_t bench_ref, std::uint32_t keyword) const = 0;
    /// The KYWD with this editor id, 0 if none.
    [[nodiscard]] virtual std::uint32_t keyword(std::string_view editor_id) const = 0;
    /// The model of the bottle created potions use; empty if none.
    [[nodiscard]] virtual std::string potion_model() const { return {}; }
    virtual void log(const std::string& line) = 0;
};

class Crafting {
public:
    Crafting(const wfb::World& world, CraftingHost& host, vm::Inventory& inventory, vm::ItemExtras& extras,
             vm::CreatedForms& created, std::uint32_t player)
        : world_(world), host_(host), inventory_(inventory), extras_(extras), created_(created), player_(player) {}

    /// The create-object recipes of the bench (type 1), listed ones.
    [[nodiscard]] std::vector<RecipeRow> recipes(std::uint32_t bench_ref) const;
    /// Make `count` times the recipe (as many as the components allow); the
    /// number made.
    std::int64_t craft(std::uint32_t recipe, std::int64_t count);

    /// Tempering rows of a smithing bench (type 2 or 7): one per carried
    /// stack of a product of a listed recipe.
    [[nodiscard]] std::vector<TemperRow> temper_rows(std::uint32_t bench_ref) const;
    /// Temper one unit of the stack (form, extra) with `recipe`: takes the
    /// components, and the unit becomes one with the tier's health. False if
    /// it cannot (components, no stack, no improvement).
    bool temper(std::uint32_t recipe, std::uint32_t form, std::uint32_t extra);

    /// The carried ingredients with their effects and what is known.
    [[nodiscard]] std::vector<AlchemyRow> alchemy_rows() const;
    /// What mixing `forms` would make.
    [[nodiscard]] AlchemyPreview preview(std::span<const std::uint32_t> forms) const;
    /// Mix `forms` (one of each is consumed, all must be carried); the
    /// created potion's form, 0 if it cannot be done.
    std::uint32_t mix(std::span<const std::uint32_t> forms);
    /// The player ate `ingredient`: its first effect becomes known.
    void note_eaten(std::uint32_t ingredient);

    /// The text of a magic effect with `<mag>` and `<dur>` filled in.
    [[nodiscard]] std::string describe(std::uint32_t effect, float magnitude, std::uint32_t duration) const;
    [[nodiscard]] std::string effect_name(std::uint32_t effect) const;
    /// The tier an item at `health` shows as ("" if untempered).
    [[nodiscard]] static const char* tier_name(float health) { return vm::temper_tier_name(health); }

private:
    [[nodiscard]] bool listed(const wfb::Recipe& recipe, const ConditionItem* item) const;
    [[nodiscard]] std::vector<Component> components_of(const wfb::Recipe& recipe) const;
    [[nodiscard]] static bool all_held(const std::vector<Component>& components);
    [[nodiscard]] float health_of(std::uint32_t extra) const;
    [[nodiscard]] bool enchanted(std::uint32_t form, std::uint32_t extra) const;
    void take_components(const std::vector<Component>& components);
    void log_skill(const char* skill, std::uint32_t form, std::int64_t value);
    [[nodiscard]] std::int64_t value_of(std::uint32_t form) const;

    const wfb::World& world_;
    CraftingHost& host_;
    vm::Inventory& inventory_;
    vm::ItemExtras& extras_;
    vm::CreatedForms& created_;
    std::uint32_t player_;
};

} // namespace skydot::crafting
