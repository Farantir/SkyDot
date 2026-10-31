// SPDX-License-Identifier: GPL-3.0-or-later
//
// What scripts and the game did to actors' values, kept sparse per reference
// and saved as the ACTV section. Value = base + modifier + damage (damage is
// 0 or less); max = base + modifier + temporary (the magic bucket: what active
// magic effects hold, never saved, the effects re-add it on load). The base comes from the data (an
// `ActorBase`, asked for through a callback and cached, never saved) unless a
// script set it. Health, Magicka and Stamina regenerate in `tick`; a
// non-essential, non-player actor at Health 0 is dead. Nothing here touches
// Godot.
#pragma once

#include "save/bytes.hpp"
#include "vm/actor_values.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace skydot::vm {

class ActorStates {
public:
    static constexpr std::uint32_t k_section_version = 7;
    /// Seconds Magicka and Stamina wait after taking damage before they
    /// regenerate: the GMSTs fDamagedMagickaRegenDelay and
    /// fDamagedStaminaRegenDelay (0.5 in Skyrim.esm). These are the fallbacks
    /// for a world without them; `set_regen_delays` takes the world's.
    static constexpr double k_damaged_magicka_regen_delay = 0.5;
    static constexpr double k_damaged_stamina_regen_delay = 0.5;
    void set_regen_delays(double magicka, double stamina) {
        magicka_regen_delay_ = magicka;
        stamina_regen_delay_ = stamina;
    }
    /// Regeneration in combat: the GMSTs fCombatStaminaRegenRateMult and
    /// fCombatMagickaRegenRateMult (0.35 and 0.33 in Skyrim.esm), and who is in combat.
    /// Health in combat is scaled by fCombatHealthRegenRateMult (0.0 in Skyrim.esm: no regeneration),
    /// or by the actor's CombatHealthRegenMultiply when that is set (above 0).
    static constexpr double k_combat_health_regen_mult = 0.7;
    static constexpr double k_combat_stamina_regen_mult = 0.35;
    static constexpr double k_combat_magicka_regen_mult = 0.33;
    void set_combat_regen(double health, double magicka, double stamina) {
        combat_health_regen_mult_ = health;
        combat_magicka_regen_mult_ = magicka;
        combat_stamina_regen_mult_ = stamina;
    }
    void set_in_combat(std::function<bool(std::uint32_t ref)> in_combat) { in_combat_ = std::move(in_combat); }

    /// Where a dead actor lies (version 2 of the section): the space (cell or
    /// worldspace form), the position in Skyrim units and the facing about Z
    /// in radians.
    struct CorpsePlace {
        std::uint32_t space = 0;
        double x = 0.0, y = 0.0, z = 0.0;
        double rot_z = 0.0;
    };

    /// Fills `out` with the data's values for actor `ref`; false if `ref` is
    /// no actor.
    using BaseSource = std::function<bool(std::uint32_t ref, ActorBase& out)>;

    void set_source(BaseSource source) { source_ = std::move(source); }

    /// Whether `ref` is an actor the data knows.
    [[nodiscard]] bool is_actor(std::uint32_t ref) const { return base_of(ref) != nullptr; }

    /// Everything below is 0 / a no-op for a reference that is no actor or
    /// an index outside the table.
    [[nodiscard]] double get(std::uint32_t ref, std::uint32_t av) const;
    [[nodiscard]] double get_base(std::uint32_t ref, std::uint32_t av) const;
    /// Base plus modifier.
    [[nodiscard]] double max(std::uint32_t ref, std::uint32_t av) const;
    /// Value over max; 1 if max is 0 or less.
    [[nodiscard]] double percentage(std::uint32_t ref, std::uint32_t av) const;
    [[nodiscard]] bool is_dead(std::uint32_t ref) const;
    /// Bring a dead actor back to full health (reanimate). PLACEHOLDER: not saved, so an actor that
    /// started dead is dead again after a load.
    void revive(std::uint32_t ref);
    /// Who dealt the blow that killed `ref` (0 if unknown or alive); kept in
    /// the save.
    void set_killer(std::uint32_t ref, std::uint32_t killer);
    [[nodiscard]] std::uint32_t killer_of(std::uint32_t ref) const;
    void set_corpse_place(std::uint32_t ref, const CorpsePlace& place);
    [[nodiscard]] std::optional<CorpsePlace> corpse_place(std::uint32_t ref) const;
    /// Every actor with a corpse place, ascending.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, CorpsePlace>> corpse_places() const;
    /// Where a living actor stood when the game was saved (version 7): kept apart from the corpse's place, put back
    /// by the load. `clear_living_places` drops them all (they are captured again at the next save).
    void set_living_place(std::uint32_t ref, const CorpsePlace& place);
    void clear_living_places() { living_.clear(); }
    [[nodiscard]] std::optional<CorpsePlace> living_place(std::uint32_t ref) const;
    [[nodiscard]] const std::map<std::uint32_t, CorpsePlace>& living_places() const { return living_; }
    /// Every actor that is dead, ascending.
    [[nodiscard]] std::vector<std::uint32_t> dead_refs() const;

    /// SetAllowFlying (version 3 of the section): whether a flying actor may fly; true unless a script
    /// said otherwise.
    void set_allowed_to_fly(std::uint32_t ref, bool allowed);
    [[nodiscard]] bool allowed_to_fly(std::uint32_t ref) const;
    /// SetForcedLandingMarker (version 3): the reference a flying actor lands at; 0 none.
    void set_landing_marker(std::uint32_t ref, std::uint32_t marker);
    [[nodiscard]] std::uint32_t landing_marker(std::uint32_t ref) const;

    /// Dying (version 5 of the section): an actor whose OnDeath waits until its body is at rest (a
    /// death clip, a crash). `seconds` is the longest it waits; `tick` counts it down.
    void defer_death(std::uint32_t ref, double seconds);
    /// The body is at rest: true if OnDeath was waiting (it is not any more).
    bool finish_dying(std::uint32_t ref);
    [[nodiscard]] bool is_dying(std::uint32_t ref) const { return dying_.contains(ref); }
    /// The actors whose wait ran out since the last call.
    [[nodiscard]] std::vector<std::uint32_t> take_due_deaths();

    /// SetGhost (version 4 of the section): a ghost takes no hits.
    void set_ghost(std::uint32_t ref, bool ghost);
    [[nodiscard]] bool is_ghost(std::uint32_t ref) const;
    /// Kill (Actor.Kill): Health to 0 and the actor dead whatever its flags, `killer` (0: none) recorded;
    /// it joins `take_deaths` as any death does. The player only loses his health (nobody dies in the VM
    /// for him). Nothing for a dead actor.
    void kill(std::uint32_t ref, std::uint32_t killer);

    /// What a script said about an `ActorBase` (version 6): SetEssential and SetInvulnerable, kept by the
    /// NPC_ and winning over the data. Essential: Health 0 does not kill. Invulnerable: Health takes no damage.
    void set_base_essential(std::uint32_t npc, bool essential);
    void set_base_invulnerable(std::uint32_t npc, bool invulnerable);
    [[nodiscard]] bool is_essential(std::uint32_t ref) const;
    [[nodiscard]] bool is_invulnerable(std::uint32_t ref) const;

    /// Relationship ranks between two actors, in the Papyrus scale (4 lover ... 0 acquaintance ...
    /// -4 archnemesis), the same both ways round (version 6). None if no script set one.
    void set_relationship_rank(std::uint32_t a, std::uint32_t b, std::int32_t rank);
    [[nodiscard]] std::optional<std::int32_t> relationship_rank(std::uint32_t a, std::uint32_t b) const;

    /// Faction relations a script set (version 6): how members of `from` see members of `to` (FACT
    /// XNAM reactions: 0 neutral, 1 enemy, 2 ally, 3 friend). They win over the data's entry.
    void set_faction_reaction(std::uint32_t from, std::uint32_t to, std::uint8_t reaction);
    [[nodiscard]] std::optional<std::uint8_t> faction_reaction(std::uint32_t from, std::uint32_t to) const;
    /// Every (to, reaction) a script set for `from`.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint8_t>> faction_reactions_from(std::uint32_t from) const;
    /// Whether any faction relation was set (the AI skips its lookups otherwise).
    [[nodiscard]] bool has_faction_reactions() const { return !reactions_.empty(); }

    /// Factions a script changed (version 4): the rank it gave `ref` in `faction`; a rank below 0 is
    /// "not a member" (also against the NPC_'s own factions). Nothing for an actor that is no actor.
    void set_faction_rank(std::uint32_t ref, std::uint32_t faction, std::int32_t rank);
    /// What a script said about `faction` for `ref` (the rank, -1 for removed); none if it said nothing.
    [[nodiscard]] std::optional<std::int32_t> faction_override(std::uint32_t ref, std::uint32_t faction) const;
    /// The factions a script put `ref` in (rank 0 or more), ascending.
    [[nodiscard]] std::vector<std::uint32_t> added_factions(std::uint32_t ref) const;

    void set_base(std::uint32_t ref, std::uint32_t av, double value);
    void mod(std::uint32_t ref, std::uint32_t av, double amount);
    /// The temporary (magic) modifier: counts in value and max, is not saved. The sum an
    /// effect holds is added and, when it ends, taken away again.
    void mod_temp(std::uint32_t ref, std::uint32_t av, double amount);
    [[nodiscard]] double get_temp(std::uint32_t ref, std::uint32_t av) const;
    /// Change the modifier so that the value is `value`.
    void force(std::uint32_t ref, std::uint32_t av, double value);
    /// Lower the value by `amount` (> 0). Health, Magicka and Stamina stop at 0.
    void damage(std::uint32_t ref, std::uint32_t av, double amount);
    /// Take damage back by `amount` (> 0), at most to none.
    void restore(std::uint32_t ref, std::uint32_t av, double amount);

    /// Regenerate by `seconds` (real time).
    void tick(double seconds);
    /// The actors that died since the last call.
    [[nodiscard]] std::vector<std::uint32_t> take_deaths();

    /// Drop everything known about `ref`.
    void forget(std::uint32_t ref);
    void clear();
    /// Take the records of `other` (a loaded section), keeping this object's
    /// source; the cache of bases starts empty.
    void adopt(ActorStates&& other);

    [[nodiscard]] std::vector<std::uint8_t> save() const;
    /// Decode a section into a fresh object; false if it is cut short,
    /// names an actor value that does not exist or has bytes left over.
    /// Version 1 has no killer and no corpse place; versions below 3 no flight settings; below 4 no ghost and no factions; below 5 no dying actors; below 6 no base flags, relationships or faction relations; below 7 no living places.
    [[nodiscard]] static bool load(save::ByteReader r, ActorStates& out, std::uint32_t version = k_section_version);

private:
    struct Value {
        std::optional<double> base; ///< Set by a script; else the data's.
        double modifier = 0.0;
        double temp = 0.0; ///< Not saved.
        double damage = 0.0;
    };
    struct Record {
        std::map<std::uint32_t, Value> values;
        bool dead = false;
        bool risen = false; ///< Revived: not dead although the base starts dead. Not saved.
        std::uint32_t killer = 0;
        std::optional<CorpsePlace> corpse;
        bool no_flying = false;          ///< SetAllowFlying(false).
        std::uint32_t landing_marker = 0;
        bool ghost = false;                              ///< SetGhost(true).
        std::map<std::uint32_t, std::int32_t> factions;  ///< Script-set ranks; -1 removed.
        double magicka_pause = 0.0; ///< Not saved.
        double stamina_pause = 0.0;
    };

    [[nodiscard]] const ActorBase* base_of(std::uint32_t ref) const;
    [[nodiscard]] double base_value(std::uint32_t ref, const Record* record, std::uint32_t av) const;
    [[nodiscard]] double modifier_value(const Record* record, std::uint32_t av) const;
    [[nodiscard]] double damage_value(const Record* record, std::uint32_t av) const;
    [[nodiscard]] const Record* find(std::uint32_t ref) const;
    /// The record of `ref`, made; null if `ref` is no actor or `av` is not valid.
    Record* touch(std::uint32_t ref, std::uint32_t av);
    void check_death(std::uint32_t ref, Record& record);

    BaseSource source_;
    double magicka_regen_delay_ = k_damaged_magicka_regen_delay;
    double stamina_regen_delay_ = k_damaged_stamina_regen_delay;
    double combat_magicka_regen_mult_ = k_combat_magicka_regen_mult;
    double combat_health_regen_mult_ = k_combat_health_regen_mult;
    double combat_stamina_regen_mult_ = k_combat_stamina_regen_mult;
    std::function<bool(std::uint32_t ref)> in_combat_;
    mutable std::map<std::uint32_t, std::optional<ActorBase>> bases_;
    std::map<std::uint32_t, Record> records_;
    std::vector<std::uint32_t> deaths_;
    std::map<std::uint32_t, double> dying_;  ///< Seconds OnDeath still waits; saved.
    std::vector<std::uint32_t> due_;
    struct BaseFlags {
        std::optional<bool> essential;
        std::optional<bool> invulnerable;
    };
    std::map<std::uint32_t, BaseFlags> base_flags_;                    ///< By NPC_; saved.
    std::map<std::uint64_t, std::int32_t> relationships_;              ///< (low ref << 32 | high ref); saved.
    std::map<std::uint64_t, std::uint8_t> reactions_;
    std::map<std::uint32_t, CorpsePlace> living_;                      ///< Where living actors stood at the save.                  ///< (from << 32 | to); saved.
};

} // namespace skydot::vm
