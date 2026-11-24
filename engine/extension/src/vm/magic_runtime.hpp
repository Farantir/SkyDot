// SPDX-License-Identifier: GPL-3.0-or-later
//
// The magic domain of the Papyrus runtime: the active effects, spell books, shouts and worn
// enchantments (magic/, docs/adr/0008) over the VM's actor values, inventory and scripts. It is the
// host the effects act through (MagicHost) and the API the natives call (MagicApi,
// vm/natives_magic.hpp). Events of effects (a start, a finish) are queued while the rules run and
// sent afterwards (`flush`), because a script that answers one may dispel another effect. No Godot:
// it reaches the engine through MagicRuntime::Host, ItemRuntime and CombatRuntime.
#pragma once

#include "conditions/conditions.hpp"
#include "magic/active_effects.hpp"
#include "magic/enchanted.hpp"
#include "magic/finish_grace.hpp"
#include "magic/shouts.hpp"
#include "magic/spell_lists.hpp"
#include "save/bytes.hpp"
#include "state/actor_states.hpp"
#include "state/created_forms.hpp"
#include "state/inventory.hpp"
#include "state/runtime_refs.hpp"
#include "vm/activation.hpp"
#include "vm/combat_runtime.hpp"
#include "vm/item_runtime.hpp"
#include "vm/natives_magic.hpp"
#include "vm/vm.hpp"
#include "vm/world_access.hpp"
#include "world_generated.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skydot::vm {

namespace wfb = bethconv::pack::wfb;

/// "0x%X" and "%g", as the magic log lines write ids and numbers.
inline std::string magic_hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%X", v);
    return buf;
}
inline std::string magic_number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

class MagicRuntime final : public magic::MagicHost, public MagicApi {
public:
    class Host {
    public:
        virtual ~Host() = default;
        [[nodiscard]] virtual WorldAccess& world_access() = 0;
        [[nodiscard]] virtual Inventory& inventory() = 0;
        [[nodiscard]] virtual ActorStates& actor_states() = 0;
        /// `value` after the perks of `owner` at perk entry point `ep` (perks/entry_points.hpp).
        [[nodiscard]] virtual double perk_entry(std::uint8_t /*ep*/, std::uint32_t /*owner*/,
                                                std::array<std::uint32_t, 3> /*subjects*/, double value) const {
            return value;
        }
        /// The abilities of the perks `ref` has from its NPC_ (PRKR), which are part of its base spell list.
        [[nodiscard]] virtual std::vector<std::uint32_t> perk_abilities(std::uint32_t /*ref*/) { return {}; }
        [[nodiscard]] virtual CreatedForms& created_forms() = 0;
        [[nodiscard]] virtual Vm& vm() = 0;
        /// A condition world of its own for one evaluation.
        [[nodiscard]] virtual std::unique_ptr<conditions::ConditionWorld> new_condition_world() = 0;
        /// Forget everything the VM keeps of `ref` (it is gone).
        virtual void forget_ref(std::uint32_t ref) = 0;
        /// The NPC_ an actor is (a leveled list's pick, its template's stats); 0 if `ref` is no actor.
        [[nodiscard]] virtual std::uint32_t dialogue_npc(std::uint32_t ref) const = 0;
        /// Place a reference (a summon); 0 if it could not be.
        virtual std::uint32_t place_summon_ref(const PlacedRef& placed) = 0;
        /// The actors inside trigger volumes now, the ones the AI reports.
        [[nodiscard]] virtual std::vector<std::uint32_t> triggered_actors() const = 0;
        /// A line for the player's trace (the signal), then for the log.
        virtual void magic_trace(const std::string& line) = 0;
        /// A notification for the player.
        virtual void magic_notification(const std::string& text) = 0;
        /// An effect started or ended: the signals magic_effect_changed and magic_effect_event.
        virtual void effect_event(const magic::EffectView& view, bool started) = 0;
        /// The Papyrus scripts of MGEF `mgef` attach to the script object `id`.
        virtual void attach_effect_scripts(std::uint32_t id, std::uint32_t mgef) = 0;
    };

    MagicRuntime(Host& host, const wfb::World& world, ItemRuntime& items, CombatRuntime& combat);

    /// The magic stores keyed by reference or by effect instance id, in the VM's registry (state/ref_forget.hpp).
    /// The tokens end with this state, which a new VM's setup resets first.
    void register_forgets();

    /// PLACEHOLDER: how far in front of the caster a summon appears, Skyrim units.
    static constexpr double k_summon_distance = 150.0;
    static std::uint32_t actor_level_of(const wfb::Npc& npc);

    Host* host_;
    const wfb::World& world_;
    ItemRuntime& items_;
    CombatRuntime& combat_;
    magic::ActiveEffects effects;
    magic::SpellBook book;
    /// Words of power, shouts and the voice slot (shared with the power of `book`).
    magic::ShoutBook shouts;
    /// The constant effects of worn enchanted armor (never saved: derived when an actor is primed).
    magic::WornEnchantments worn;

    struct Pending {
        bool started;
        magic::EffectView view;
    };
    std::vector<Pending> pending;
    bool flushing = false;
    /// Effects that have script objects: handle -> the last view, kept until the object is gone.
    std::map<std::uint32_t, magic::EffectView> snapshots;
    /// Handles to detach, once the VM seconds of grace passed (a script gets its OnEffectFinish first).
    magic::FinishGrace detach_later;
    bool abilities_primed = false;
    std::vector<vm::ForgetRegistry::Registration> forgets;

    // ---- MagicHost
    double actor_value(std::uint32_t ref, std::uint32_t av) const override { return host_->actor_states().get(ref, av); }
    void damage_value(std::uint32_t ref, std::uint32_t av, double amount, std::uint32_t caster) override;
    void restore_value(std::uint32_t ref, std::uint32_t av, double amount) override {
        host_->actor_states().restore(ref, av, amount);
    }
    void modify_temp(std::uint32_t ref, std::uint32_t av, double delta) override {
        host_->actor_states().mod_temp(ref, av, delta);
    }
    bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list, std::uint32_t subject,
                         std::uint32_t caster) override;
    bool is_dead(std::uint32_t ref) const override { return host_->actor_states().is_dead(ref); }
    double game_setting(std::string_view name, double fallback) const override;
    void effect_changed(bool started, const magic::EffectView& e) override { pending.push_back({started, e}); }

    // ---- archetypes (UESP; placeholders marked in magic/active_effects.cpp)
    std::uint32_t actor_level(std::uint32_t ref) const override;
    std::uint32_t place_summon(std::uint32_t caster, std::uint32_t npc_base) override;
    void remove_summon(std::uint32_t ref) override;
    void reanimate(std::uint32_t target, std::uint32_t caster) override;
    void end_reanimate(std::uint32_t target) override;
    void bound_add(std::uint32_t target, std::uint32_t weapon) override;
    void bound_remove(std::uint32_t target, std::uint32_t weapon) override;
    std::vector<std::uint32_t> actors_near(std::uint32_t ref, double radius) const override;
    void log(const std::string& line) override { host_->magic_trace(line); }

    // ---- helpers
    [[nodiscard]] magic::MagicSettings settings() const;
    /// The text of a text GMST, or `fallback`.
    [[nodiscard]] std::string game_text(std::string_view name, const char* fallback) const;
    [[nodiscard]] std::string word_name(std::uint32_t word) const;
    void message(const std::string& text) { host_->magic_notification(text); }
    [[nodiscard]] std::string spell_name(std::uint32_t spell) const;
    void prime(std::uint32_t actor);
    /// What `actor` wears with an enchantment (armor, clothing, jewelry and a shield; a weapon's is
    /// not constant) is on it as constant effects; nothing on the dead.
    void sync_worn(std::uint32_t actor);
    /// A scroll item `actor` carries.
    [[nodiscard]] bool carried_scroll(std::uint32_t actor, std::uint32_t form);
    /// The effects of an ALCH, INGR, SCRL or created potion; `poison` if it is one.
    bool item_effects(std::uint32_t form, std::vector<magic::EffectInput>& inputs, magic::SourceKind& kind,
                      bool& poison);
    static magic::SourceKind kind_of(const wfb::Spell& s);

    /// Put the effects of `spell` on `target`; the ids made.
    std::vector<std::uint32_t> land(std::uint32_t caster, std::uint32_t blame, std::uint32_t spell,
                                    std::uint32_t target);

    // ---- MagicApi
    bool cast(std::uint32_t spell, std::uint32_t caster, std::uint32_t blame, std::uint32_t target) override;
    double spell_cost(std::uint32_t spell, std::uint32_t caster) override;
    double perk_entry(std::uint8_t ep, std::uint32_t owner, std::array<std::uint32_t, 3> subjects,
                      double value) const override {
        return host_->perk_entry(ep, owner, subjects, value);
    }
    bool add_spell(std::uint32_t actor, std::uint32_t spell, bool verbose) override;
    bool remove_spell(std::uint32_t actor, std::uint32_t spell) override;
    bool has_spell(std::uint32_t actor, std::uint32_t spell) override {
        return host_->actor_states().is_actor(actor) && book.has_spell(actor, spell);
    }
    bool dispel_spell(std::uint32_t actor, std::uint32_t spell) override;
    void dispel_all_spells(std::uint32_t actor) override;
    bool has_magic_effect(std::uint32_t actor, std::uint32_t mgef) override;
    bool has_magic_effect_keyword(std::uint32_t actor, std::uint32_t keyword) override;
    static magic::Slot slot_of(int hand) {
        return hand == 0 ? magic::Slot::left : (hand == 1 ? magic::Slot::right : magic::Slot::power);
    }
    bool equip_spell(std::uint32_t actor, std::uint32_t spell, int hand) override;
    void unequip_spell(std::uint32_t actor, int hand) override { book.unequip(actor, slot_of(hand)); }
    std::uint32_t equipped_spell(std::uint32_t actor, int hand) override { return book.equipped(actor, slot_of(hand)); }
    // ---- shouts
    bool teach_word(std::uint32_t word) override;
    bool unlock_word(std::uint32_t word) override { return shouts.unlock_word(k_player_ref, word); }
    bool is_word_unlocked(std::uint32_t word) override { return shouts.is_word_unlocked(k_player_ref, word); }
    bool add_shout(std::uint32_t actor, std::uint32_t shout) override {
        return host_->actor_states().is_actor(actor) && shouts.add_shout(actor, shout);
    }
    bool remove_shout(std::uint32_t actor, std::uint32_t shout) override { return shouts.remove_shout(actor, shout); }
    bool equip_shout(std::uint32_t actor, std::uint32_t shout) override;
    void unequip_shout(std::uint32_t actor, std::uint32_t shout) override;
    std::uint32_t equipped_shout(std::uint32_t actor) override { return shouts.equipped(actor); }
    void set_voice_recovery(std::uint32_t actor, double seconds) override { shouts.set_recovery(actor, seconds); }
    double voice_recovery(std::uint32_t actor) override { return shouts.recovery_left(actor); }

    std::optional<Effect> effect(std::uint32_t handle) override;
    void dispel_effect(std::uint32_t handle) override;

    // ---- events, ticks, deaths, the save section (magic_runtime.cpp)
    /// Send what effects started or finished since the last call (the signals, Papyrus events).
    void flush();
    /// Advance the effects by `seconds` (game seconds for effects, real for the voice, VM for script objects).
    void tick(double seconds);
    /// Whether the player has a Soul Trap on `ref` (the soul goes into a gem as it dies).
    [[nodiscard]] bool soul_trapped_by_player(std::uint32_t ref) const;
    /// `ref` died: its effects end, its worn enchantments go, the events are sent.
    void actor_died(std::uint32_t ref);
    /// Break the invisibility of `ref`; how many effects ended.
    std::int64_t break_invisibility(std::uint32_t ref);
    /// Dispel the bound weapon `weapon` of `actor`; whether any effect did.
    bool dispel_bound_weapon(std::uint32_t actor, std::uint32_t weapon);
    /// Drop all effects, books and pending events.
    void clear();
    /// The MAGC section's bytes.
    [[nodiscard]] std::vector<std::uint8_t> save() const;
    /// The parts of a MAGC section, checked against the pack; false if it is not valid.
    static bool decode(const wfb::World& world, save::ByteReader r, std::vector<std::uint8_t>& effects,
                       std::vector<std::uint8_t>& book, std::vector<std::uint8_t>& shouts);
    /// Take a decoded section (`has` false: none) and start the abilities and worn enchantments again.
    void adopt(bool has, const std::vector<std::uint8_t>& effects, const std::vector<std::uint8_t>& book,
               const std::vector<std::uint8_t>& shouts);
    // ---- worn enchantments, poisons, enchanted weapons (magic_runtime.cpp)
    /// What `owner` wears with an enchantment follows what it wears; the events are sent.
    void sync_worn_magic(std::uint32_t owner);
    /// One unit of `form` of `owner`'s changes from the extra `from` to `to`, equipment and all.
    void move_unit_extra(std::uint32_t owner, std::uint32_t form, std::uint32_t from, std::uint32_t to);
    void retarget_equipment(std::uint32_t owner, std::uint32_t form, std::uint32_t from, std::uint32_t to);
    /// What a weapon hit did besides damage (the poison and enchantment of the weapon).
    struct HitMagic {
        std::uint32_t poison = 0, enchantment = 0;
        bool fired = false;
        double charge = 0.0, charge_max = 0.0;
    };
    HitMagic hit_magic(std::uint32_t attacker, std::uint32_t target, std::uint32_t weapon, std::uint32_t extra,
                       bool target_alive);
    /// The answer of putting a poison on the weapon in the right hand.
    struct PoisonResult {
        bool ok = false;
        std::string reason;
        std::uint32_t weapon = 0;
        int doses = 0;
    };
    PoisonResult poison_weapon(std::uint32_t actor, std::uint32_t form, std::int64_t extra);
    /// The charge of the weapon `form` of `actor` (the unit `extra`; -1: the one in hand, else the first).
    struct WeaponCharge {
        std::uint32_t enchantment = 0;
        double charge = 0.0, max = 0.0, cost = 0.0;
    };
    WeaponCharge weapon_charge(std::uint32_t actor, std::uint32_t form, std::int64_t extra);
    /// Bind the spell, effect, active-effect, actor-magic and shout natives on `vm`.
    void bind_natives(Vm& vm);
};

} // namespace skydot::vm
