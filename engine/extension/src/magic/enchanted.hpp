// SPDX-License-Identifier: GPL-3.0-or-later
//
// Enchanted and poisoned items (GAMEPLAY-PLAN 7a-3, docs/adr/0008): what a weapon's enchantment
// costs per hit and how its charge is spent, what a hit does with the enchantment and the poison on
// the weapon, and the constant effects of worn enchanted armor. Sources: UESP "Skyrim:Enchanting"
// (a weapon spends the enchantment's cost per hit; an NPC's enchanted weapon never runs out) and
// "Skyrim:Poison" (a poison goes on the equipped weapon, the next hit delivers it). Pure: the
// inventory and the equipment are the caller's, this decides and reports the new extra data.
// Nothing here touches Godot.
#pragma once

#include "magic/active_effects.hpp"
#include "vm/item_extras.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace skydot::magic {

/// PLACEHOLDER (P6): the Enchanting skill and perks scale the charge a hit costs; x1, logged.
inline constexpr double k_placeholder_enchant_cost_scale = 1.0;

/// PLACEHOLDER (P6): hits one poison gives (perks add doses).
inline constexpr std::uint32_t k_placeholder_poison_doses = 1;

/// ENIT flag 0x1: the cost is the stored one; clear: the sum of the effects' costs.
inline constexpr std::uint32_t k_ench_manual_cost = 0x1;

/// What one hit with the enchantment costs: ENIT cost when manual, else the sum of `effect_cost` of
/// its effects (fire and forget: a weapon enchantment is one), times the placeholder.
[[nodiscard]] double enchantment_cost(const wfb::World& world, const wfb::Enchantment& ench,
                                      const MagicSettings& s = {});

/// The most a weapon holds: the extra's (a created enchantment), else the base's EAMT, else the ENCH's charge.
[[nodiscard]] float charge_max(float extra_max, std::uint32_t item_amount, std::int32_t enchantment_charge);
/// The charge held: the full-charge sentinel is `max`; clamped to 0..max.
[[nodiscard]] float charge_now(float stored, float max);

struct ChargeSpend {
    bool effect = false; ///< The enchantment fires.
    float after = 0.0F;  ///< The charge left.
    double cost = 0.0;
};
/// A hit with `stored` charge of `max` and an enchantment costing `cost`: it fires while any charge is
/// left (a last, short charge still fires) and takes `cost`, down to 0. `spends` false (an NPC's
/// weapon) never takes anything.
[[nodiscard]] ChargeSpend spend_charge(float stored, float max, double cost, bool spends);

/// The EFID/EFIT entries of a poison form (an ALCH, a created potion); false if it is none.
using PoisonInputs = std::function<bool(std::uint32_t poison, std::vector<EffectInput>& out)>;

struct HitMagicInput {
    std::uint32_t attacker = 0;
    std::uint32_t target = 0;
    std::uint32_t weapon = 0;
    std::uint32_t extra = 0; ///< The extra id of the weapon unit (0: plain).
    bool attacker_is_player = false;
    bool target_alive = true; ///< Effects do not land on a corpse; charge and poison are still spent.
};

struct HitMagicResult {
    std::uint32_t poison = 0;      ///< The poison delivered (0: none).
    std::uint32_t enchantment = 0; ///< The enchantment of the weapon (0: none), fired or not.
    bool fired = false;            ///< The enchantment's effects landed.
    float charge = 0.0F;           ///< Charge after the hit (0 without an enchantment).
    float max = 0.0F;
    /// The weapon unit's extra id after the hit; differs from the input when charge or poison changed.
    std::uint32_t new_extra = 0;
    std::vector<std::uint32_t> instances;
};

/// The magic of one weapon hit: deliver and spend the poison, fire the enchantment and spend charge.
/// The effects come from the attacker onto the target (poison: SourceKind::poison, source the poison
/// form; enchantment: SourceKind::enchantment, source the weapon, extra 0 so a repeated hit refreshes).
HitMagicResult hit_magic(const wfb::World& world, ActiveEffects& effects, vm::ItemExtras& extras,
                         const HitMagicInput& in, const PoisonInputs& poison_inputs, const MagicSettings& s = {});

/// A unit's enchantment: the extra's, else the base item's; 0 if none.
[[nodiscard]] std::uint32_t enchantment_of(const wfb::World& world, std::uint32_t form, const vm::ItemExtra* extra);
/// Charge, max and cost per hit of a weapon unit (all 0 without an enchantment).
struct WeaponCharge {
    std::uint32_t enchantment = 0;
    float charge = 0.0F;
    float max = 0.0F;
    double cost = 0.0;
};
[[nodiscard]] WeaponCharge weapon_charge(const wfb::World& world, std::uint32_t form, const vm::ItemExtra* extra,
                                         const MagicSettings& s = {});

/// A poison put on a unit: the extra after (the poison and `doses` hits), as it was if it carried one already.
[[nodiscard]] bool can_poison(const vm::ItemExtra& extra) noexcept;
[[nodiscard]] vm::ItemExtra poisoned(vm::ItemExtra extra, std::uint32_t poison, std::uint32_t doses = 1);

// ---- worn enchantments ------------------------------------------------------------------------

/// A worn piece with an enchantment.
struct WornEnchant {
    std::uint32_t item = 0;
    std::uint32_t extra = 0;
    std::uint32_t enchantment = 0;
    friend bool operator==(const WornEnchant&, const WornEnchant&) = default;
};

/// The constant effects of worn enchanted armor, clothing and jewelry (weapons' are not constant):
/// SourceKind::enchantment with the item as source, held until the piece comes off. Never saved: a
/// load clears them (`ActiveEffects` does not write them) and `sync` derives them again.
class WornEnchantments {
public:
    WornEnchantments(const wfb::World& world, ActiveEffects& effects) : world_(world), effects_(effects) {}

    /// Make `worn` what `actor` has on: start what is new, end what is gone or changed.
    void sync(std::uint32_t actor, const std::vector<WornEnchant>& worn);
    /// Forget `actor`'s state without ending anything (the effects went with a death or a load).
    void forget(std::uint32_t actor) { applied_.erase(actor); }
    void clear() { applied_.clear(); }
    [[nodiscard]] std::vector<WornEnchant> applied(std::uint32_t actor) const;

private:
    const wfb::World& world_;
    ActiveEffects& effects_;
    std::map<std::uint32_t, std::map<std::uint32_t, WornEnchant>> applied_; ///< Actor -> item -> piece.
};

} // namespace skydot::magic
