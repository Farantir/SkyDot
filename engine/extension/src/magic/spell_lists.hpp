// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which spells an actor knows, which it holds in each hand, when a power may be used again and
// which enchantments the player has learned (docs/adr/0008). The known spells of an actor are what
// its NPC_ and race list (leveled spell lists resolved, seeded by the reference, so an actor draws
// the same spells every time) plus the ones a script added, minus the ones it removed. Abilities
// (SPEL type 4) are constant effects while known: they are applied when an actor is first looked at
// and are never saved, they are derived again. Nothing here touches Godot.
#pragma once

#include "magic/active_effects.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace skydot::magic {

enum class Slot : std::uint8_t { left = 0, right = 1, power = 2 };

/// The spells of NPC_ `npc` placed as `ref`, plus its race's: SPLO as they stand, an LVSP resolved
/// for a player of `level`, a shout (SHOU, not in world.fb) dropped; each spell once, in order. A
/// template that gives the spell list (flag 0x8) is followed.
[[nodiscard]] std::vector<std::uint32_t> base_spells(const wfb::World& world, std::uint32_t npc, std::uint32_t ref,
                                                     std::uint16_t level);

class SpellBook {
public:
    static constexpr std::uint32_t k_section_version = 1;
    /// A power can be used again after this many game days.
    static constexpr double k_power_cooldown_days = 1.0;

    /// The resolved base spells of an actor reference (NPC_ + race); empty for a non-actor.
    using BaseSource = std::function<std::vector<std::uint32_t>(std::uint32_t ref)>;

    SpellBook(const wfb::World& world, ActiveEffects& effects) : world_(world), effects_(effects) {}
    void set_base_source(BaseSource source) { source_ = std::move(source); }

    /// Known spells of `ref`: base + added - removed. The base is asked once per actor and kept.
    [[nodiscard]] std::vector<std::uint32_t> spells_of(std::uint32_t ref);
    [[nodiscard]] bool has_spell(std::uint32_t ref, std::uint32_t spell);
    /// Teach a spell (false if it is known already or is no SPEL); an ability starts at once.
    bool add_spell(std::uint32_t ref, std::uint32_t spell);
    /// Make `ref` forget a spell (false if it did not know it); an ability ends, a held spell is put away.
    bool remove_spell(std::uint32_t ref, std::uint32_t spell);

    /// Apply the abilities `ref` knows, once (idempotent until `reset_abilities`).
    void ensure_abilities(std::uint32_t ref);
    /// After a load: nothing is applied any more (the effects were replaced).
    void reset_abilities() { applied_.clear(); }
    /// Actors with anything of their own here, for re-deriving their abilities.
    [[nodiscard]] std::vector<std::uint32_t> refs() const;

    [[nodiscard]] std::uint32_t equipped(std::uint32_t ref, Slot slot) const;
    /// Hold `spell` in `slot` (false if `ref` does not know it). A power goes to the power slot only.
    /// With `scroll` the form is a scroll the caller has checked `ref` carries (a scroll is an item,
    /// not a known spell): any form is held in a hand; the slot keeps it (MAGC saves it as it is).
    bool equip(std::uint32_t ref, std::uint32_t spell, Slot slot, bool scroll = false);
    void unequip(std::uint32_t ref, Slot slot);

    /// Whether `spell` may be used by `ref` at `game_days` (powers once per game day; anything else always).
    [[nodiscard]] bool ready(std::uint32_t ref, std::uint32_t spell, double game_days) const;
    void note_used(std::uint32_t ref, std::uint32_t spell, double game_days);
    /// Game days until a power is ready again (0 if it is).
    [[nodiscard]] double cooldown_left(std::uint32_t ref, std::uint32_t spell, double game_days) const;

    bool learn_enchantment(std::uint32_t enchantment);
    [[nodiscard]] bool knows_enchantment(std::uint32_t enchantment) const { return known_enchantments_.contains(enchantment); }
    [[nodiscard]] const std::set<std::uint32_t>& known_enchantments() const { return known_enchantments_; }

    void forget(std::uint32_t ref);
    void clear();
    [[nodiscard]] std::vector<std::uint8_t> save() const;
    [[nodiscard]] static bool load(save::ByteReader r, SpellBook& out);
    /// Take the state of `other` (a decoded section), keeping this object's source.
    void adopt(SpellBook&& other);

private:
    struct Record {
        std::set<std::uint32_t> added;
        std::set<std::uint32_t> removed;
        std::uint32_t slots[3] = {0, 0, 0};
        std::map<std::uint32_t, double> used; ///< Power -> game day of its last use.
    };
    [[nodiscard]] const std::vector<std::uint32_t>& base_of(std::uint32_t ref);
    [[nodiscard]] std::uint32_t type_of(std::uint32_t spell) const;
    void apply_ability(std::uint32_t ref, std::uint32_t spell);

    const wfb::World& world_;
    ActiveEffects& effects_;
    BaseSource source_;
    std::map<std::uint32_t, Record> records_;
    std::map<std::uint32_t, std::vector<std::uint32_t>> base_cache_;
    std::set<std::uint32_t> applied_; ///< Not saved.
    std::set<std::uint32_t> known_enchantments_;
};

/// The actors whose abilities and worn enchantments are derived again after a load (`magic_adopt`):
/// the player, everyone the spell book has a record for (spells added or removed, equipped spells,
/// power uses), everyone an effect runs on, and `wearers` (owners of worn items, for armor
/// enchantments). Pure, so a test needs no pack.
[[nodiscard]] std::set<std::uint32_t> actors_to_rederive(const SpellBook& book, const ActiveEffects& effects,
                                                         const std::vector<std::uint32_t>& wearers = {});

} // namespace skydot::magic
