// SPDX-License-Identifier: GPL-3.0-or-later
//
// Extra data of single items (docs/adr/0007): what makes one Iron Dagger
// different from another. A value, interned by the session: id 0 is "plain",
// ids from 1 name distinct extras and are never reused. The table is saved
// (ITST). Nothing here touches Godot.
#pragma once

#include "save/bytes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace skydot::vm {

struct ItemExtra {
    /// Tempering: 1.0 untouched, up to 1.6 (Legendary).
    float health = 1.0F;
    /// An Enchantment of the world, 0 if none (the base's own is separate).
    std::uint32_t enchantment = 0;
    /// A name the player gave it (enchanting); empty keeps the base's.
    std::string name;

    [[nodiscard]] bool plain() const noexcept;
    [[nodiscard]] bool same(const ItemExtra& other) const noexcept;
};

/// The tempering tiers: health at or above `health` (within a hundredth) is
/// named `name`. A health below the first is untempered.
struct TemperTier {
    float health;
    const char* name;
};
inline constexpr TemperTier k_temper_tiers[] = {
    {1.1F, "Fine"}, {1.2F, "Superior"}, {1.3F, "Exquisite"}, {1.4F, "Flawless"}, {1.5F, "Epic"}, {1.6F, "Legendary"},
};

/// The tier a health stands for ("" if untempered).
[[nodiscard]] const char* temper_tier_name(float health) noexcept;

class ItemExtras {
public:
    /// The id of `extra`, added if new; 0 for a plain one.
    std::uint32_t intern(const ItemExtra& extra);
    /// The extra of `id`; null for 0 and ids this table does not have.
    [[nodiscard]] const ItemExtra* find(std::uint32_t id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return table_.size(); }
    void clear() { table_.clear(); }

    void write(save::ByteWriter& w) const;
    [[nodiscard]] static bool read(save::ByteReader& r, ItemExtras& out);

private:
    std::vector<ItemExtra> table_; ///< Id = index + 1.
};

} // namespace skydot::vm
