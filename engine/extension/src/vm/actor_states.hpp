// SPDX-License-Identifier: GPL-3.0-or-later
//
// What scripts and the game did to actors' values, kept sparse per reference
// and saved as the ACTV section. Value = base + modifier + damage (damage is
// 0 or less); max = base + modifier. The base comes from the data (an
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
    static constexpr std::uint32_t k_section_version = 2;
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
    /// Who dealt the blow that killed `ref` (0 if unknown or alive); kept in
    /// the save.
    void set_killer(std::uint32_t ref, std::uint32_t killer);
    [[nodiscard]] std::uint32_t killer_of(std::uint32_t ref) const;
    void set_corpse_place(std::uint32_t ref, const CorpsePlace& place);
    [[nodiscard]] std::optional<CorpsePlace> corpse_place(std::uint32_t ref) const;
    /// Every actor with a corpse place, ascending.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, CorpsePlace>> corpse_places() const;
    /// Every actor that is dead, ascending.
    [[nodiscard]] std::vector<std::uint32_t> dead_refs() const;

    void set_base(std::uint32_t ref, std::uint32_t av, double value);
    void mod(std::uint32_t ref, std::uint32_t av, double amount);
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
    /// Version 1 has no killer and no corpse place.
    [[nodiscard]] static bool load(save::ByteReader r, ActorStates& out, std::uint32_t version = k_section_version);

private:
    struct Value {
        std::optional<double> base; ///< Set by a script; else the data's.
        double modifier = 0.0;
        double damage = 0.0;
    };
    struct Record {
        std::map<std::uint32_t, Value> values;
        bool dead = false;
        std::uint32_t killer = 0;
        std::optional<CorpsePlace> corpse;
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
    mutable std::map<std::uint32_t, std::optional<ActorBase>> bases_;
    std::map<std::uint32_t, Record> records_;
    std::vector<std::uint32_t> deaths_;
};

} // namespace skydot::vm
