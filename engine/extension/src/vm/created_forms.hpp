// SPDX-License-Identifier: GPL-3.0-or-later
//
// Forms made at run time (docs/adr/0007): the potions and poisons that
// alchemy creates, with ids from id_ranges::created_forms, deduplicated by
// their effects (mixing the same effects again gives the same form), and what
// the player has learned about ingredients (a 4-bit mask of known effects
// each). Section CRFT of a save (optional: an older save has nothing created
// and nothing known). Nothing here touches Godot.
//
//   CRFT  (version 2 adds the enchantments after the ingredients: u32 enchantments, each u32 id, text
//         name, u32 base, f64 cost, u8 constant, u32 effects (u32 effect, f64 magnitude, u32 duration,
//         u32 area))
//         u32 next id; u32 potions, each u32 id, text name, i32 value, f64
//         weight, u8 poison, text model, u32 effects (u32 effect, f64
//         magnitude, u32 duration); u32 ingredients, each u32 form, u8 mask
#pragma once

#include "save/bytes.hpp"
#include "vm/id_ranges.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace skydot::vm {

struct CreatedEffect {
    std::uint32_t effect = 0; ///< A MagicEffect of the world.
    float magnitude = 0.0F;
    std::uint32_t duration = 0;
    std::uint32_t area = 0; ///< Feet; 0 for potions.

    [[nodiscard]] bool operator==(const CreatedEffect& other) const noexcept;
};

struct CreatedPotion {
    std::uint32_t id = 0;
    std::string name;
    std::vector<CreatedEffect> effects;
    std::int32_t value = 0;
    float weight = 0.5F;
    bool poison = false;
    /// The world model of a vanilla potion bottle, empty if none was found.
    std::string model;
};

/// An enchantment the player made at the table (docs/adr/0008): the effects of a known world
/// enchantment (`base`) at the strength the soul and skill gave, saved by value.
struct CreatedEnchantment {
    std::uint32_t id = 0;
    std::uint32_t base = 0; ///< The world ENCH it was made from.
    std::string name;
    std::vector<CreatedEffect> effects;
    /// Charge one use costs (a weapon's); 0 for apparel.
    double cost = 0.0;
    /// Constant effect (apparel); clear: fire and forget on a hit (weapons).
    bool constant = false;

    [[nodiscard]] bool operator==(const CreatedEnchantment& other) const noexcept;
};

class CreatedForms {
public:
    /// 1 potions and ingredients; 2 adds the created enchantments.
    static constexpr std::uint32_t k_section_version = 2;

    /// The form of `potion`: an existing one with the same effects and
    /// poison flag, else a new one (`potion.id` is set). 0 when the range is
    /// used up.
    std::uint32_t create(CreatedPotion potion);
    [[nodiscard]] const CreatedPotion* find(std::uint32_t id) const;
    /// The id of `ench`: an existing one with the same base, effects, cost and kind, else a new one
    /// (`ench.id` is set). 0 when the range is used up.
    std::uint32_t create_enchantment(CreatedEnchantment ench);
    [[nodiscard]] const CreatedEnchantment* find_enchantment(std::uint32_t id) const;
    [[nodiscard]] const std::map<std::uint32_t, CreatedEnchantment>& enchantments() const { return enchantments_; }
    [[nodiscard]] bool is_created(std::uint32_t id) const noexcept {
        return id_ranges::created_forms.contains(id);
    }
    [[nodiscard]] const std::map<std::uint32_t, CreatedPotion>& potions() const { return potions_; }

    /// Bit `index` of the ingredient's mask: its effect `index` is known.
    [[nodiscard]] bool known(std::uint32_t ingredient, std::size_t index) const;
    [[nodiscard]] std::uint8_t known_mask(std::uint32_t ingredient) const;
    void learn(std::uint32_t ingredient, std::size_t index);
    [[nodiscard]] const std::map<std::uint32_t, std::uint8_t>& knowledge() const { return known_; }

    void clear();
    void adopt(CreatedForms&& other) { *this = std::move(other); }

    [[nodiscard]] std::vector<std::uint8_t> save() const;
    /// `version` is the section's (1 reads an old save: nothing is enchanted).
    [[nodiscard]] static bool load(save::ByteReader r, CreatedForms& out, std::uint32_t version = k_section_version);

private:
    IdAllocator ids_{id_ranges::created_forms};
    std::map<std::uint32_t, CreatedPotion> potions_;
    std::map<std::uint32_t, CreatedEnchantment> enchantments_;
    std::map<std::uint32_t, std::uint8_t> known_;
};

} // namespace skydot::vm
