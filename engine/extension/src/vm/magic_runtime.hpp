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
#include "data/actors.hpp"
#include "data/fb_search.hpp"
#include "data/leveled.hpp"
#include "data/world_data.hpp"
#include "magic/active_effects.hpp"
#include "magic/casting.hpp"
#include "magic/enchanted.hpp"
#include "magic/finish_grace.hpp"
#include "magic/magic_save.hpp"
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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
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

    MagicRuntime(Host& host, const wfb::World& world, ItemRuntime& items, CombatRuntime& combat)
        : host_(&host), world_(world), items_(items), combat_(combat), effects(world, *this), book(world, effects), shouts(world),
          worn(world, effects, &host.created_forms()) {
        shouts.set_base_source([this](std::uint32_t ref) {
            if (!host_->actor_states().is_actor(ref)) {
                return std::vector<std::uint32_t>{};
            }
            const std::uint32_t npc = host_->dialogue_npc(ref);
            return npc != 0 ? magic::base_shouts(world_, npc, ref, leveled::player_level())
                            : std::vector<std::uint32_t>{};
        });
        book.set_base_source([this](std::uint32_t ref) {
            if (!host_->actor_states().is_actor(ref)) {
                return std::vector<std::uint32_t>{};
            }
            const std::uint32_t npc = host_->dialogue_npc(ref);
            if (npc == 0) {
                return std::vector<std::uint32_t>{};
            }
            auto spells = magic::base_spells(world_, npc, ref, leveled::player_level());
            for (const std::uint32_t ability : host_->perk_abilities(ref)) {
                if (std::ranges::find(spells, ability) == spells.end()) {
                    spells.push_back(ability);
                }
            }
            return spells;
        });
        register_forgets();
    }

    /// The magic stores keyed by reference or by effect instance id, in the VM's registry (state/ref_forget.hpp).
    /// The tokens end with this state, which a new VM's setup resets first.
    void register_forgets() {
        auto& registry = host_->vm().forgets();
        forgets.push_back(registry.add(
            "magic.effects",
            [this](std::uint32_t ref) {
                // The effects on a gone actor end without being undone; their script objects go with them.
                for (const std::uint32_t id : effects.forget(ref)) {
                    host_->vm().detach(id);
                    snapshots.erase(id);
                }
            },
            [this] { return effects.entries(); },
            // Casters, targets and summoned refs of the held effects, of the views waiting to be sent and of
            // the snapshots of effects with script objects: values the forget does not scrub.
            [this](std::set<std::uint32_t>& out) {
                effects.held_refs(out);
                for (const auto& p : pending) {
                    out.insert({p.view.caster, p.view.target, p.view.summoned});
                }
                for (const auto& [id, view] : snapshots) {
                    out.insert({view.caster, view.target, view.summoned});
                }
            }));
        forgets.push_back(registry.add(
            "magic.spell_book", [this](std::uint32_t ref) { book.forget(ref); }, [this] { return book.entries(); }));
        forgets.push_back(registry.add(
            "magic.shouts", [this](std::uint32_t ref) { shouts.forget(ref); }, [this] { return shouts.entries(); }));
        forgets.push_back(registry.add(
            "magic.worn", [this](std::uint32_t ref) { worn.forget(ref); }, [this] { return worn.entries(); }));
        forgets.push_back(registry.add(
            "magic.pending",
            [this](std::uint32_t ref) {
                std::erase_if(pending, [ref](const Pending& p) { return p.view.target == ref; });
            },
            [this] { return pending.size(); }));
        // Keyed by effect instance id (the VM's own store drops the script objects).
        forgets.push_back(registry.add(
            "magic.script_effects",
            [this](std::uint32_t id) {
                snapshots.erase(id);
                detach_later.forget(id);
            },
            [this] { return snapshots.size() + detach_later.size(); }));
    }

    /// PLACEHOLDER: how far in front of the caster a summon appears, Skyrim units.
    static constexpr double k_summon_distance = 150.0;
    static std::uint32_t actor_level_of(const wfb::Npc& npc) { return skydot::actor_level(npc); }

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
    void damage_value(std::uint32_t ref, std::uint32_t av, double amount, std::uint32_t caster) override {
        if (av == vm::av::k_health && amount > 0.0 && combat_.ghost_ignores_hit(ref)) {
            return;
        }
        host_->actor_states().damage(ref, av, amount);
        if (caster != 0 && av == vm::av::k_health) {
            if (host_->actor_states().get(ref, vm::av::k_health) <= 0.0 && host_->actor_states().killer_of(ref) == 0) {
                host_->actor_states().set_killer(ref, caster);
            }
            if (ref == k_player_ref) {
                combat_.set_last_damager(caster);
            }
        }
    }
    void restore_value(std::uint32_t ref, std::uint32_t av, double amount) override {
        host_->actor_states().restore(ref, av, amount);
    }
    void modify_temp(std::uint32_t ref, std::uint32_t av, double delta) override {
        host_->actor_states().mod_temp(ref, av, delta);
    }
    bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list, std::uint32_t subject,
                         std::uint32_t caster) override {
        const auto cw = host_->new_condition_world();
        conditions::ConditionContext ctx;
        ctx.subject = subject;
        ctx.target = caster;
        ctx.unknown = conditions::UnknownPolicy::fails;
        return conditions::conditions_pass(list, *cw, ctx, nullptr);
    }
    bool is_dead(std::uint32_t ref) const override { return host_->actor_states().is_dead(ref); }
    double game_setting(std::string_view name, double fallback) const override {
        return static_cast<double>(WorldData::gmst_float(&world_, name, static_cast<float>(fallback)));
    }
    void effect_changed(bool started, const magic::EffectView& e) override { pending.push_back({started, e}); }

    // ---- archetypes (UESP; placeholders marked in magic/active_effects.cpp)
    std::uint32_t actor_level(std::uint32_t ref) const override {
        if (ref == k_player_ref) {
            return leveled::player_level();
        }
        if (!host_->actor_states().is_actor(ref)) {
            return 0;
        }
        const auto* npc = resolve_npc(world_, host_->dialogue_npc(ref), wfb::NpcTemplateFlags::use_stats, ref);
        return npc != nullptr ? actor_level_of(*npc) : 0;
    }
    std::uint32_t place_summon(std::uint32_t caster, std::uint32_t npc_base) override {
        PlacedRef placed;
        placed.base = npc_base;
        placed.cell = host_->world_access().parent_cell(caster);
        const Xyz position = host_->world_access().ref_position(caster);
        const Xyz rotation = host_->world_access().ref_rotation(caster);
        // PLACEHOLDER: 150 units in front of the caster (rotation z is the heading, radians).
        const double heading = rotation[2];
        placed.position[0] = position[0] + std::sin(heading) * k_summon_distance;
        placed.position[1] = position[1] + std::cos(heading) * k_summon_distance;
        placed.position[2] = position[2];
        for (std::size_t i = 0; i < 3; ++i) {
            placed.rotation[i] = rotation[i];
        }
        placed.scale = 1.0;
        const std::uint32_t ref = host_->place_summon_ref(placed);
        if (ref == 0 || !host_->actor_states().is_actor(ref)) {
            if (ref != 0) {
                host_->forget_ref(ref);
            }
            return 0;
        }
        log("magic: " + magic_hex(npc_base) + " summoned " + magic_hex(ref) + " for " + magic_hex(caster));
        return ref;
    }
    void remove_summon(std::uint32_t ref) override {
        log("magic: summon " + magic_hex(ref) + " gone");
        host_->forget_ref(ref);
    }
    void reanimate(std::uint32_t target, std::uint32_t caster) override {
        host_->actor_states().revive(target);
        log("magic: " + magic_hex(target) + " reanimated for " + magic_hex(caster));
    }
    void end_reanimate(std::uint32_t target) override {
        // PLACEHOLDER: it just dies (UESP: it turns to ash).
        host_->actor_states().damage(target, vm::av::k_health, 1e9);
    }
    void bound_add(std::uint32_t target, std::uint32_t weapon) override {
        // PLACEHOLDER: always the right hand (the MGEF names no hand).
        if (items_.equip_item(target, weapon, false, true)) {
            log("magic: " + magic_hex(target) + " bound weapon " + magic_hex(weapon) + " added");
        } else {
            log("magic: " + magic_hex(target) + " cannot equip bound weapon " + magic_hex(weapon));
        }
    }
    void bound_remove(std::uint32_t target, std::uint32_t weapon) override {
        (void)items_.unequip_item(target, weapon);
        (void)items_.remove_item(target, weapon, 1);
        log("magic: " + magic_hex(target) + " bound weapon " + magic_hex(weapon) + " removed");
    }
    std::vector<std::uint32_t> actors_near(std::uint32_t ref, double radius) const override {
        std::vector<std::uint32_t> out;
        const auto float_position = [this](std::uint32_t actor) {
            const Xyz p = host_->world_access().ref_position(actor);
            return std::array<float, 3>{static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
        };
        const auto at = float_position(ref);
        const auto consider = [&](std::uint32_t actor) {
            if (actor == ref || !host_->actor_states().is_actor(actor) || host_->actor_states().is_dead(actor)) {
                return;
            }
            // The distance as the float vector math of the engine rounds it.
            const auto p = float_position(actor);
            const float dx = p[0] - at[0];
            const float dy = p[1] - at[1];
            const float dz = p[2] - at[2];
            if (static_cast<double>(std::sqrt(dx * dx + dy * dy + dz * dz)) <= radius) {
                out.push_back(actor);
            }
        };
        for (const std::uint32_t actor : host_->triggered_actors()) {
            if (actor != k_player_ref) {
                consider(actor);
            }
        }
        consider(k_player_ref);
        return out;
    }
    void log(const std::string& line) override { host_->magic_trace(line); }

    // ---- helpers
    [[nodiscard]] magic::MagicSettings settings() const {
        magic::MagicSettings s;
        s.max_resistance = game_setting("fPlayerMaxResistance", s.max_resistance);
        s.skill_cost_exponent = game_setting("fMagicPCSkillCostScale", s.skill_cost_exponent);
        s.area_cost_mult = game_setting("fMagicAreaBaseCostMult", s.area_cost_mult);
        return s;
    }
    /// The text of a text GMST, or `fallback`.
    [[nodiscard]] std::string game_text(std::string_view name, const char* fallback) const {
        const auto* g = WorldData::find_gmst(&world_, name);
        return g != nullptr && g->text_value() != nullptr && g->text_value()->size() > 0 ? g->text_value()->str()
                                                                                         : std::string(fallback);
    }
    [[nodiscard]] std::string word_name(std::uint32_t word) const {
        const auto* w = lookup(world_.words_of_power(), word);
        return w != nullptr && w->name() != nullptr ? magic::word_spelling(w->name()->str()) : magic_hex(word);
    }
    void message(const std::string& text) {
        host_->magic_notification(text);
    }
    [[nodiscard]] std::string spell_name(std::uint32_t spell) const {
        const auto* s = lookup(world_.spells(), spell);
        return s != nullptr && s->name() != nullptr ? s->name()->str() : magic_hex(spell);
    }
    void prime(std::uint32_t actor) {
        if (host_->actor_states().is_actor(actor)) {
            book.ensure_abilities(actor);
            sync_worn(actor);
        }
    }
    /// What `actor` wears with an enchantment (armor, clothing, jewelry and a shield; a weapon's is
    /// not constant) is on it as constant effects; nothing on the dead.
    void sync_worn(std::uint32_t actor) {
        if (!host_->actor_states().is_actor(actor)) {
            return;
        }
        if (host_->actor_states().is_dead(actor)) {
            worn.forget(actor);
            return;
        }
        items_.resolve_owner(actor);
        std::vector<magic::WornEnchant> list;
        if (const vm::Equipment* eq = items_.states().find(actor)) {
            const auto add = [&](std::uint32_t form, std::uint32_t extra) {
                const vm::ItemInfo info = items_.item_info_of(form);
                if (!info.known || (info.category != vm::ItemCategory::armor && info.category != vm::ItemCategory::clothing &&
                                    info.category != vm::ItemCategory::jewelry)) {
                    return;
                }
                if (const std::uint32_t ench = magic::enchantment_of(world_, form, items_.states().extras().find(extra))) {
                    list.push_back({form, extra, ench});
                }
            };
            for (const std::uint32_t form : eq->worn) {
                add(form, eq->worn_extra_of(form));
            }
            if (eq->left != 0 && vm::is_shield(items_.item_info_of(eq->left))) {
                add(eq->left, eq->left_extra);
            }
        }
        worn.sync(actor, list);
    }
    /// A scroll item `actor` carries.
    [[nodiscard]] bool carried_scroll(std::uint32_t actor, std::uint32_t form) {
        const auto* base = lookup(world_.bases(), form);
        const auto* item = base != nullptr ? base->item() : nullptr;
        if (item == nullptr || item->category() != wfb::ItemCategory::scroll) {
            return false;
        }
        items_.resolve_owner(actor);
        return host_->inventory().count(actor, form) > 0;
    }
    /// The effects of an ALCH, INGR, SCRL or created potion; `poison` if it is one.
    bool item_effects(std::uint32_t form, std::vector<magic::EffectInput>& inputs, magic::SourceKind& kind,
                      bool& poison) {
        kind = magic::SourceKind::potion;
        poison = false;
        const auto* base = lookup(world_.bases(), form);
        const auto* item = base != nullptr ? base->item() : nullptr;
        if (item != nullptr) {
            inputs = magic::inputs_of(item->effects());
            switch (item->category()) {
            case wfb::ItemCategory::ingredient: kind = magic::SourceKind::ingredient; break;
            case wfb::ItemCategory::scroll: kind = magic::SourceKind::scroll; break;
            case wfb::ItemCategory::potion:
                // The pack has no ALCH poison flag (ENIT 0x20000): a potion whose effects are all hostile is one.
                if (!item->food() && !inputs.empty()) {
                    poison = std::ranges::all_of(inputs, [&](const magic::EffectInput& in) {
                        const auto* m = lookup(world_.magic_effects(), in.effect);
                        return m != nullptr && (m->flags() & magic::mgef_flag::hostile) != 0;
                    });
                }
                break;
            default: break;
            }
            return true;
        }
        if (const auto* created = host_->created_forms().find(form)) {
            poison = created->poison;
            std::int32_t index = 0;
            for (const auto& e : created->effects) {
                inputs.push_back({e.effect, e.magnitude, 0, e.duration, index++});
            }
            return true;
        }
        return false;
    }
    static magic::SourceKind kind_of(const wfb::Spell& s) {
        switch (s.type()) {
        case magic::spell_type::ability: return magic::SourceKind::ability;
        case magic::spell_type::poison: return magic::SourceKind::poison;
        default: return s.casting_type() == magic::casting::scroll ? magic::SourceKind::scroll : magic::SourceKind::spell;
        }
    }

    /// Put the effects of `spell` on `target`; the ids made.
    std::vector<std::uint32_t> land(std::uint32_t caster, std::uint32_t blame, std::uint32_t spell,
                                    std::uint32_t target) {
        const auto* s = lookup(world_.spells(), spell);
        if (s == nullptr) {
            return {};
        }
        const std::uint32_t source_actor = blame != 0 ? blame : caster;
        prime(source_actor);
        prime(target);
        const auto inputs = magic::inputs_of(s->effects());
        auto ids = effects.apply(source_actor, target, {kind_of(*s), spell, 0}, inputs);
        return ids;
    }

    // ---- MagicApi
    bool cast(std::uint32_t spell, std::uint32_t caster, std::uint32_t blame, std::uint32_t target) override {
        if (lookup(world_.spells(), spell) == nullptr || caster == 0) {
            return false;
        }
        const std::uint32_t on = target != 0 ? target : caster;
        log("magic: " + magic_hex(blame != 0 ? blame : caster) + " casts " + magic_hex(spell) + " on " + magic_hex(on) + " (script)");
        land(caster, blame, spell, on);
        flush();
        return true;
    }
    double spell_cost(std::uint32_t spell, std::uint32_t caster) override {
        const auto* s = lookup(world_.spells(), spell);
        if (s == nullptr) {
            return 0.0;
        }
        if (caster == 0) {
            return static_cast<double>(s->cost());
        }
        const int school = magic::spell_school(world_, *s);
        const double skill = school >= 0 ? host_->actor_states().get(caster, static_cast<std::uint32_t>(school)) : 0.0;
        return magic::spell_cost(world_, *s, skill, settings());
    }
    bool add_spell(std::uint32_t actor, std::uint32_t spell, bool verbose) override {
        if (!host_->actor_states().is_actor(actor)) {
            return false;
        }
        prime(actor);
        const bool added = book.add_spell(actor, spell);
        if (added && verbose && actor == k_player_ref) {
            message(spell_name(spell) + " added");
        }
        flush();
        return added;
    }
    bool remove_spell(std::uint32_t actor, std::uint32_t spell) override {
        prime(actor);
        const bool removed = book.remove_spell(actor, spell);
        flush();
        return removed;
    }
    bool has_spell(std::uint32_t actor, std::uint32_t spell) override {
        return host_->actor_states().is_actor(actor) && book.has_spell(actor, spell);
    }
    bool dispel_spell(std::uint32_t actor, std::uint32_t spell) override {
        const bool any = effects.dispel(actor, spell) != 0;
        flush();
        return any;
    }
    void dispel_all_spells(std::uint32_t actor) override {
        effects.dispel_all(actor);
        flush();
    }
    bool has_magic_effect(std::uint32_t actor, std::uint32_t mgef) override {
        prime(actor);
        return effects.has_effect(actor, mgef);
    }
    bool has_magic_effect_keyword(std::uint32_t actor, std::uint32_t keyword) override {
        prime(actor);
        return effects.has_effect_keyword(actor, keyword);
    }
    static magic::Slot slot_of(int hand) {
        return hand == 0 ? magic::Slot::left : (hand == 1 ? magic::Slot::right : magic::Slot::power);
    }
    bool equip_spell(std::uint32_t actor, std::uint32_t spell, int hand) override {
        if (hand == 2) {
            const bool changed = shouts.equipped(actor) != spell && book.equipped(actor, magic::Slot::power) != spell;
            const bool done = magic::equip_voice(book, shouts, actor, spell); // the voice slot: a power or a shout
            if (done && changed) {
                items_.send_equip_event(actor, spell, true);
            }
            return done;
        }
        const bool scroll = !book.has_spell(actor, spell) && carried_scroll(actor, spell);
        const bool changed = equipped_spell(actor, hand) != spell;
        const bool done = book.equip(actor, spell, slot_of(hand), scroll);
        if (done && changed) {
            items_.send_equip_event(actor, spell, true);
        }
        return done;
    }
    void unequip_spell(std::uint32_t actor, int hand) override { book.unequip(actor, slot_of(hand)); }
    std::uint32_t equipped_spell(std::uint32_t actor, int hand) override { return book.equipped(actor, slot_of(hand)); }
    // ---- shouts
    bool teach_word(std::uint32_t word) override {
        const bool learned = shouts.teach_word(k_player_ref, word);
        if (learned) {
            message(game_text("sShoutAdded", "Word of Power Learned") + ": " + word_name(word));
        }
        return learned;
    }
    bool unlock_word(std::uint32_t word) override { return shouts.unlock_word(k_player_ref, word); }
    bool is_word_unlocked(std::uint32_t word) override { return shouts.is_word_unlocked(k_player_ref, word); }
    bool add_shout(std::uint32_t actor, std::uint32_t shout) override {
        return host_->actor_states().is_actor(actor) && shouts.add_shout(actor, shout);
    }
    bool remove_shout(std::uint32_t actor, std::uint32_t shout) override { return shouts.remove_shout(actor, shout); }
    bool equip_shout(std::uint32_t actor, std::uint32_t shout) override {
        if (!shouts.is_shout(shout)) {
            return false;
        }
        const bool changed = shouts.equipped(actor) != shout;
        const bool done = magic::equip_voice(book, shouts, actor, shout);
        if (done && changed) {
            items_.send_equip_event(actor, shout, true);
        }
        return done;
    }
    void unequip_shout(std::uint32_t actor, std::uint32_t shout) override {
        if (shout == 0 || shouts.equipped(actor) == shout) {
            shouts.unequip(actor);
        }
    }
    std::uint32_t equipped_shout(std::uint32_t actor) override { return shouts.equipped(actor); }
    void set_voice_recovery(std::uint32_t actor, double seconds) override { shouts.set_recovery(actor, seconds); }
    double voice_recovery(std::uint32_t actor) override { return shouts.recovery_left(actor); }

    std::optional<Effect> effect(std::uint32_t handle) override {
        if (const auto live = effects.instance(handle)) {
            return Effect{live->mgef, live->target, live->caster, live->magnitude, live->duration, live->elapsed};
        }
        const auto snap = snapshots.find(handle);
        if (snap == snapshots.end()) {
            return std::nullopt;
        }
        const auto& v = snap->second;
        return Effect{v.mgef, v.target, v.caster, v.magnitude, v.duration, v.elapsed};
    }
    void dispel_effect(std::uint32_t handle) override {
        effects.dispel_instance(handle);
        flush();
    }

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
