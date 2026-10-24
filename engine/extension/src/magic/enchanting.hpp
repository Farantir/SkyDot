// SPDX-License-Identifier: GPL-3.0-or-later
//
// The enchanting table (GAMEPLAY-PLAN 7a-6, docs/adr/0008): disenchanting an item to learn its
// enchantment, enchanting an item with a known one and a filled soul gem, recharging a weapon, and
// filling a soul gem from a dying actor. Sources: UESP "Skyrim:Enchanting_Effects" (the formulas),
// "Skyrim:Soul_Gems" (capacities), "Skyrim:Soul_Trap_(effect)" (the smallest gem that fits; an NPC
// soul only in a black gem). The Enchanting skill is the placeholder 15 and there are no perks. The
// formulas are free functions; `Enchanting` moves the units. Nothing here touches Godot.
#pragma once

#include "magic/enchanted.hpp"
#include "magic/spell_lists.hpp"
#include "vm/created_forms.hpp"
#include "vm/inventory.hpp"
#include "vm/item_extras.hpp"

#include "world_generated.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace skydot::magic {

namespace wfb = bethconv::pack::wfb;

// ---- formulas ---------------------------------------------------------------------------------

/// Soul sizes, as SLGM SOUL and SLCP: a black (NPC) soul is grand-sized.
inline constexpr std::uint8_t k_soul_petty = 1;
inline constexpr std::uint8_t k_soul_grand = 5;

/// ENCH enchantment types: 6 an item enchantment, 12 a staff's.
inline constexpr std::uint32_t k_ench_type_item = 6;
inline constexpr std::uint32_t k_ench_type_staff = 12;
/// ENCH casting types.
inline constexpr std::uint32_t k_ench_casting_constant = 0;
inline constexpr std::uint32_t k_ench_casting_fire_and_forget = 1;

/// 1 + (skill/100) * (skill/100 - 0.14) / 3.4.
[[nodiscard]] double enchant_skill_multiplier(double skill);
/// The charge a soul of this size gives: 250, 500, 1000, 2000, 3000 (0 for none).
[[nodiscard]] double soul_charge(std::uint8_t soul);
/// What an apparel enchantment's magnitude takes from the soul: 1/12, 1/6, 1/3, 2/3, 1.
[[nodiscard]] double soul_magnitude_multiplier(std::uint8_t soul);
/// Charge per use of a weapon enchantment: 3 * base_cost^1.1 * (1 - sqrt(skill/200)).
[[nodiscard]] double weapon_use_cost(double base_cost, double skill);

/// One effect of the base enchantment at the strength `factor` gives: the magnitude is
/// floor(magnitude * factor), at least 1; an effect whose MGEF has flags `power affects duration`
/// and not `power affects magnitude` scales its duration instead.
[[nodiscard]] vm::CreatedEffect scale_effect(const EffectInput& base, std::uint32_t mgef_flags, double factor);

/// What the soul of a dying actor is.
struct ActorSoul {
    std::uint8_t size = 0;
    bool npc = false; ///< Only a gem that holds NPC souls takes it.
};
/// A creature's soul by level (UESP's bands: petty 1-3, lesser 4-15, common 16-27, greater 28-37,
/// grand 38 and up); an NPC's is black and grand-sized.
[[nodiscard]] ActorSoul actor_soul(std::uint32_t level, bool is_npc);

// ---- items and gems ---------------------------------------------------------------------------

/// The keywords the rules look for (ids, 0 if the pack has none).
struct EnchantKeywords {
    std::uint32_t disallow_enchanting = 0; ///< MagicDisallowEnchanting
    std::uint32_t reusable_soul_gem = 0;   ///< ReusableSoulGem
};

enum class ItemKind : std::uint8_t { none, weapon, staff, apparel };
/// A weapon that is no staff and no fist, or armor, clothing and jewelry; `none` for anything else.
[[nodiscard]] ItemKind item_kind(const wfb::World& world, std::uint32_t form);
[[nodiscard]] bool base_has_keyword(const wfb::World& world, std::uint32_t form, std::uint32_t keyword);

/// A soul gem unit: what its base and its extra say together.
struct GemInfo {
    bool gem = false;
    std::uint8_t soul = 0;     ///< Held (the base's, else the extra's).
    std::uint8_t capacity = 0; ///< The largest it takes.
    std::uint32_t linked = 0;  ///< The filled form an empty one becomes.
    bool npc = false;          ///< Holds NPC souls.
    bool reusable = false;
};
[[nodiscard]] GemInfo gem_info(const wfb::World& world, const EnchantKeywords& kw, std::uint32_t form,
                               const vm::ItemExtra* extra);

/// Whether the world enchantment `ench` may be put on the item `form`: an item enchantment (not a
/// staff's) of the kind the item takes (constant for apparel, fire and forget for weapons), and the
/// item is one of the kinds its worn restrictions list (a FormList of keywords; 0 for any).
[[nodiscard]] bool enchantment_fits(const wfb::World& world, const wfb::Enchantment& ench, std::uint32_t form);

enum class Refusal : std::uint8_t {
    none,
    no_item,        ///< The unit is not carried.
    no_enchantment, ///< Disenchant: nothing to learn.
    created,        ///< Disenchant: a made enchantment.
    staff,
    forbidden,      ///< MagicDisallowEnchanting on the item or one of the effects.
    known,          ///< Disenchant: already learned.
    enchanted,      ///< Enchant: the item has an enchantment already.
    not_enchantable,
    unknown_enchantment, ///< Enchant: not learned (or no such ENCH).
    does_not_fit,
    no_gem,         ///< Not carried, no soul in it, or no such gem.
    full,           ///< Recharge: nothing to add.
    no_effects,
};

/// The enchantment a disenchant of the unit would teach (0 with a refusal).
struct DisenchantCheck {
    Refusal refusal = Refusal::none;
    std::uint32_t learns = 0;
};
[[nodiscard]] DisenchantCheck check_disenchant(const wfb::World& world, const EnchantKeywords& kw,
                                               const std::set<std::uint32_t>& known, std::uint32_t form,
                                               const vm::ItemExtra* extra);

/// The enchantment `base_ench` made onto `kind` with `soul` at `skill`: the created record (id 0, a
/// name of the ENCH), and the most a weapon holds (0 for apparel). Nothing if there are no effects.
struct MadeEnchantment {
    vm::CreatedEnchantment ench;
    float charge_max = 0.0F;
};
[[nodiscard]] std::optional<MadeEnchantment> make_enchantment(const wfb::World& world, const wfb::Enchantment& base_ench,
                                                              ItemKind kind, std::uint8_t soul, double skill,
                                                              const MagicSettings& s = {});

// ---- actions on the player's inventory --------------------------------------------------------

struct EnchantRequest {
    std::uint32_t item = 0;
    std::uint32_t item_extra = 0;
    std::uint32_t enchantment = 0; ///< A known world ENCH.
    std::uint32_t gem = 0;
    std::uint32_t gem_extra = 0;
    std::string name; ///< Empty: "<item name> of <enchantment name>".
};

struct EnchantOutcome {
    Refusal refusal = Refusal::none;
    std::uint32_t created = 0;    ///< The made enchantment.
    std::uint32_t new_extra = 0;  ///< The enchanted unit's extra id (the item form is unchanged).
    std::string name;
    float charge_max = 0.0F;
    double cost = 0.0;
    [[nodiscard]] bool ok() const noexcept { return refusal == Refusal::none; }
};

struct RechargeOutcome {
    Refusal refusal = Refusal::none;
    std::uint32_t new_extra = 0;
    float charge = 0.0F;
    float max = 0.0F;
    [[nodiscard]] bool ok() const noexcept { return refusal == Refusal::none; }
};

struct SoulFill {
    bool filled = false;
    std::uint32_t gem = 0;   ///< The empty gem that was used.
    std::uint32_t form = 0;  ///< What the unit is now (the linked filled form, or the same).
    std::uint32_t extra = 0; ///< Its extra id (carries the soul when the form is the same).
    std::uint8_t soul = 0;
};

class Enchanting {
public:
    Enchanting(const wfb::World& world, vm::Inventory& inventory, vm::ItemExtras& extras, vm::CreatedForms& created,
               SpellBook& book, EnchantKeywords keywords, std::uint32_t player, double skill = 15.0,
               const MagicSettings& settings = {})
        : world_(world), inventory_(inventory), extras_(extras), created_(created), book_(book), kw_(keywords),
          player_(player), skill_(skill), settings_(settings) {}

    /// Destroy one unit and learn its enchantment (the base it was made from, else itself).
    Refusal disenchant(std::uint32_t form, std::uint32_t extra);
    /// What enchanting would give; nothing is changed (the refusal is the reason it cannot).
    [[nodiscard]] EnchantOutcome preview(const EnchantRequest& req) const;
    /// Enchant one unit, spend the gem.
    EnchantOutcome enchant(const EnchantRequest& req);
    /// Add the gem's soul to a weapon's charge, spend the gem.
    RechargeOutcome recharge(std::uint32_t item, std::uint32_t item_extra, std::uint32_t gem, std::uint32_t gem_extra);
    /// An actor of the soul `soul` died under Soul Trap: fill the smallest empty gem the player carries that takes it.
    SoulFill fill_gem(ActorSoul soul);

private:
    [[nodiscard]] EnchantOutcome plan(const EnchantRequest& req, MadeEnchantment* made) const;
    /// One unit of the gem is used up: gone, or emptied if it is reusable.
    void spend_gem(std::uint32_t form, std::uint32_t extra);
    [[nodiscard]] std::uint32_t empty_form_of(std::uint32_t filled) const;

    const wfb::World& world_;
    vm::Inventory& inventory_;
    vm::ItemExtras& extras_;
    vm::CreatedForms& created_;
    SpellBook& book_;
    EnchantKeywords kw_;
    std::uint32_t player_;
    double skill_;
    MagicSettings settings_;
};

} // namespace skydot::magic
