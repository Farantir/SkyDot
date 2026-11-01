// SPDX-License-Identifier: GPL-3.0-or-later
//
// Private to the papyrus_*.cpp files of the magic glue (papyrus_magic.cpp, papyrus_casting.cpp,
// papyrus_shouts.cpp, papyrus_enchantment_effects.cpp): `MagicState`, the host the effects act
// through (MagicHost) and the API the natives call (MagicApi, vm/natives_magic.hpp), and the small
// helpers they share. Do not include it elsewhere.
#pragma once

#include "vm/papyrus.hpp"

#include "vm/condition_world.hpp"

#include "data/scripts.hpp"
#include "conditions/functions.hpp"
#include "data/fb_search.hpp"
#include "data/actors.hpp"
#include "data/leveled.hpp"
#include "data/world_data.hpp"
#include "magic/active_effects.hpp"
#include "magic/casting.hpp"
#include "magic/enchanted.hpp"
#include "magic/finish_grace.hpp"
#include "magic/magic_save.hpp"
#include "magic/shouts.hpp"
#include "magic/spell_lists.hpp"
#include "vm/natives_magic.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

using godot::Array;
using godot::Dictionary;
using godot::String;

namespace skydot {

namespace magic_glue {

namespace wfb = bethconv::pack::wfb;

inline String to_godot(const std::string& s) { return String::utf8(s.c_str(), static_cast<int>(s.size())); }

inline std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%X", v);
    return buf;
}

inline std::string number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

inline constexpr int kind_value(magic::SourceKind k) { return static_cast<int>(k); }
/// MGEF archetype 23.
inline constexpr std::uint32_t k_archetype_soul_trap = 23;


/// The common start of the API's casts: ok false with the reason when `caster` cannot act.
inline bool caster_can_act(const vm::ActorStates& actors, std::uint32_t caster, Dictionary& out) {
    if (!actors.is_actor(caster)) {
        out["reason"] = "no caster";
        return false;
    }
    if (actors.is_dead(caster)) {
        out["reason"] = "dead";
        return false;
    }
    return true;
}


} // namespace magic_glue

using namespace magic_glue;

struct SkydotPapyrus::MagicState final : SkydotPapyrus::MagicRuntime, magic::MagicHost, vm::MagicApi {
    MagicState(SkydotPapyrus& owner, const wfb::World& world)
        : owner_(owner), world_(world), effects(world, *this), book(world, effects), shouts(world), worn(world, effects, &owner.created_) {
        shouts.set_base_source([this](std::uint32_t ref) {
            if (!owner_.actors_.is_actor(ref)) {
                return std::vector<std::uint32_t>{};
            }
            const std::uint32_t npc = owner_.dialogue_npc(ref);
            return npc != 0 ? magic::base_shouts(world_, npc, ref, leveled::player_level())
                            : std::vector<std::uint32_t>{};
        });
        book.set_base_source([this](std::uint32_t ref) {
            if (!owner_.actors_.is_actor(ref)) {
                return std::vector<std::uint32_t>{};
            }
            const std::uint32_t npc = owner_.dialogue_npc(ref);
            return npc != 0 ? magic::base_spells(world_, npc, ref, leveled::player_level())
                            : std::vector<std::uint32_t>{};
        });
        register_forgets();
    }

    /// The magic stores keyed by reference or by effect instance id, in the VM's registry (state/ref_forget.hpp).
    /// The tokens end with this state, which a new VM's setup resets first.
    void register_forgets() {
        auto& registry = owner_.vm_->forgets();
        forgets.push_back(registry.add(
            "magic.effects",
            [this](std::uint32_t ref) {
                // The effects on a gone actor end without being undone; their script objects go with them.
                for (const std::uint32_t id : effects.forget(ref)) {
                    owner_.vm_->detach(id);
                    snapshots.erase(id);
                }
            },
            [this] { return effects.entries(); }));
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

    SkydotPapyrus& owner_;
    const wfb::World& world_;
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
    double actor_value(std::uint32_t ref, std::uint32_t av) const override { return owner_.actors_.get(ref, av); }
    void damage_value(std::uint32_t ref, std::uint32_t av, double amount, std::uint32_t caster) override {
        if (av == vm::av::k_health && amount > 0.0 && owner_.ghost_ignores_hit(ref)) {
            return;
        }
        owner_.actors_.damage(ref, av, amount);
        if (caster != 0 && av == vm::av::k_health) {
            if (owner_.actors_.get(ref, vm::av::k_health) <= 0.0 && owner_.actors_.killer_of(ref) == 0) {
                owner_.actors_.set_killer(ref, caster);
            }
            if (ref == k_player_ref) {
                owner_.combat_.set_last_damager(caster);
            }
        }
    }
    void restore_value(std::uint32_t ref, std::uint32_t av, double amount) override {
        owner_.actors_.restore(ref, av, amount);
    }
    void modify_temp(std::uint32_t ref, std::uint32_t av, double delta) override {
        owner_.actors_.mod_temp(ref, av, delta);
    }
    bool conditions_pass(const flatbuffers::Vector<flatbuffers::Offset<wfb::Condition>>* list, std::uint32_t subject,
                         std::uint32_t caster) override {
        PapyrusConditionWorld cw(&owner_);
        conditions::ConditionContext ctx;
        ctx.subject = subject;
        ctx.target = caster;
        ctx.unknown = conditions::UnknownPolicy::fails;
        return conditions::conditions_pass(list, cw, ctx, nullptr);
    }
    bool is_dead(std::uint32_t ref) const override { return owner_.actors_.is_dead(ref); }
    double game_setting(std::string_view name, double fallback) const override {
        return owner_.world_.is_valid()
                   ? static_cast<double>(owner_.world_->data().gmst_float(name, static_cast<float>(fallback)))
                   : fallback;
    }
    void effect_changed(bool started, const magic::EffectView& e) override { pending.push_back({started, e}); }

    // ---- archetypes (UESP; placeholders marked in magic/active_effects.cpp)
    std::uint32_t actor_level(std::uint32_t ref) const override {
        if (ref == k_player_ref) {
            return leveled::player_level();
        }
        if (!owner_.actors_.is_actor(ref)) {
            return 0;
        }
        const auto* npc = resolve_npc(world_, owner_.dialogue_npc(ref), wfb::NpcTemplateFlags::use_stats, ref);
        return npc != nullptr ? actor_level_of(*npc) : 0;
    }
    std::uint32_t place_summon(std::uint32_t caster, std::uint32_t npc_base) override {
        vm::PlacedRef placed;
        placed.base = npc_base;
        placed.cell = owner_.cell_of(caster);
        const godot::Vector3 position = owner_.position_of(caster);
        const godot::Vector3 rotation = owner_.rotation_of(caster);
        // PLACEHOLDER: 150 units in front of the caster (rotation z is the heading, radians).
        const double heading = static_cast<double>(rotation.z);
        placed.position[0] = static_cast<double>(position.x) + std::sin(heading) * k_summon_distance;
        placed.position[1] = static_cast<double>(position.y) + std::cos(heading) * k_summon_distance;
        placed.position[2] = static_cast<double>(position.z);
        for (int i = 0; i < 3; ++i) {
            placed.rotation[i] = static_cast<double>(rotation[i]);
        }
        placed.scale = 1.0;
        const std::uint32_t ref = owner_.place_ref(placed, false);
        if (ref == 0 || !owner_.actors_.is_actor(ref)) {
            if (ref != 0) {
                owner_.forget_ref(ref);
            }
            return 0;
        }
        log("magic: " + hex(npc_base) + " summoned " + hex(ref) + " for " + hex(caster));
        return ref;
    }
    void remove_summon(std::uint32_t ref) override {
        log("magic: summon " + hex(ref) + " gone");
        owner_.forget_ref(ref);
    }
    void reanimate(std::uint32_t target, std::uint32_t caster) override {
        owner_.actors_.revive(target);
        log("magic: " + hex(target) + " reanimated for " + hex(caster));
    }
    void end_reanimate(std::uint32_t target) override {
        // PLACEHOLDER: it just dies (UESP: it turns to ash).
        owner_.actors_.damage(target, vm::av::k_health, 1e9);
    }
    void bound_add(std::uint32_t target, std::uint32_t weapon) override {
        // PLACEHOLDER: always the right hand (the MGEF names no hand).
        if (owner_.item_.equip_item(target, weapon, false, true)) {
            log("magic: " + hex(target) + " bound weapon " + hex(weapon) + " added");
        } else {
            log("magic: " + hex(target) + " cannot equip bound weapon " + hex(weapon));
        }
    }
    void bound_remove(std::uint32_t target, std::uint32_t weapon) override {
        (void)owner_.item_.unequip_item(target, weapon);
        (void)owner_.item_.remove_item(target, weapon, 1);
        log("magic: " + hex(target) + " bound weapon " + hex(weapon) + " removed");
    }
    std::vector<std::uint32_t> actors_near(std::uint32_t ref, double radius) const override {
        std::vector<std::uint32_t> out;
        const godot::Vector3 at = owner_.position_of(ref);
        const auto consider = [&](std::uint32_t actor) {
            if (actor != ref && owner_.actors_.is_actor(actor) && !owner_.actors_.is_dead(actor) &&
                static_cast<double>(owner_.position_of(actor).distance_to(at)) <= radius) {
                out.push_back(actor);
            }
        };
        for (const auto& [actor, position] : owner_.triggers_.actors()) {
            if (actor != k_player_ref) {
                consider(actor);
            }
        }
        consider(k_player_ref);
        return out;
    }
    void log(const std::string& line) override {
        owner_.emit_signal("trace", to_godot(line));
        godot::UtilityFunctions::print(to_godot(line));
    }

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
        return w != nullptr && w->name() != nullptr ? magic::word_spelling(w->name()->str()) : hex(word);
    }
    void message(const std::string& text) {
        godot::UtilityFunctions::print(to_godot("[Notification] " + text));
        owner_.emit_signal("message", to_godot(text), false);
    }
    [[nodiscard]] std::string spell_name(std::uint32_t spell) const {
        const auto* s = lookup(world_.spells(), spell);
        return s != nullptr && s->name() != nullptr ? s->name()->str() : hex(spell);
    }
    void prime(std::uint32_t actor) {
        if (owner_.actors_.is_actor(actor)) {
            book.ensure_abilities(actor);
            sync_worn(actor);
        }
    }
    /// What `actor` wears with an enchantment (armor, clothing, jewelry and a shield; a weapon's is
    /// not constant) is on it as constant effects; nothing on the dead.
    void sync_worn(std::uint32_t actor) {
        if (!owner_.actors_.is_actor(actor)) {
            return;
        }
        if (owner_.actors_.is_dead(actor)) {
            worn.forget(actor);
            return;
        }
        owner_.item_.resolve_owner(actor);
        std::vector<magic::WornEnchant> list;
        if (const vm::Equipment* eq = owner_.item_.states().find(actor)) {
            const auto add = [&](std::uint32_t form, std::uint32_t extra) {
                const vm::ItemInfo info = owner_.item_.item_info_of(form);
                if (!info.known || (info.category != vm::ItemCategory::armor && info.category != vm::ItemCategory::clothing &&
                                    info.category != vm::ItemCategory::jewelry)) {
                    return;
                }
                if (const std::uint32_t ench = magic::enchantment_of(world_, form, owner_.item_.states().extras().find(extra))) {
                    list.push_back({form, extra, ench});
                }
            };
            for (const std::uint32_t form : eq->worn) {
                add(form, eq->worn_extra_of(form));
            }
            if (eq->left != 0 && vm::is_shield(owner_.item_.item_info_of(eq->left))) {
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
        owner_.item_.resolve_owner(actor);
        return owner_.vm_->inventory().count(actor, form) > 0;
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
        if (const auto* created = owner_.created_.find(form)) {
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
        owner_.emit_signal("trace", to_godot("magic: " + hex(blame != 0 ? blame : caster) + " casts " + hex(spell) +
                                             " on " + hex(on) + " (script)"));
        godot::UtilityFunctions::print(to_godot("magic: " + hex(blame != 0 ? blame : caster) + " casts " + hex(spell) +
                                                " on " + hex(on) + " (script)"));
        land(caster, blame, spell, on);
        owner_.flush_magic_events();
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
        const double skill = school >= 0 ? owner_.actors_.get(caster, static_cast<std::uint32_t>(school)) : 0.0;
        return magic::spell_cost(world_, *s, skill, settings());
    }
    bool add_spell(std::uint32_t actor, std::uint32_t spell, bool verbose) override {
        if (!owner_.actors_.is_actor(actor)) {
            return false;
        }
        prime(actor);
        const bool added = book.add_spell(actor, spell);
        if (added && verbose && actor == k_player_ref) {
            message(spell_name(spell) + " added");
        }
        owner_.flush_magic_events();
        return added;
    }
    bool remove_spell(std::uint32_t actor, std::uint32_t spell) override {
        prime(actor);
        const bool removed = book.remove_spell(actor, spell);
        owner_.flush_magic_events();
        return removed;
    }
    bool has_spell(std::uint32_t actor, std::uint32_t spell) override {
        return owner_.actors_.is_actor(actor) && book.has_spell(actor, spell);
    }
    bool dispel_spell(std::uint32_t actor, std::uint32_t spell) override {
        const bool any = effects.dispel(actor, spell) != 0;
        owner_.flush_magic_events();
        return any;
    }
    void dispel_all_spells(std::uint32_t actor) override {
        effects.dispel_all(actor);
        owner_.flush_magic_events();
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
                owner_.item_.send_equip_event(actor, spell, true);
            }
            return done;
        }
        const bool scroll = !book.has_spell(actor, spell) && carried_scroll(actor, spell);
        const bool changed = equipped_spell(actor, hand) != spell;
        const bool done = book.equip(actor, spell, slot_of(hand), scroll);
        if (done && changed) {
            owner_.item_.send_equip_event(actor, spell, true);
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
        return owner_.actors_.is_actor(actor) && shouts.add_shout(actor, shout);
    }
    bool remove_shout(std::uint32_t actor, std::uint32_t shout) override { return shouts.remove_shout(actor, shout); }
    bool equip_shout(std::uint32_t actor, std::uint32_t shout) override {
        if (!shouts.is_shout(shout)) {
            return false;
        }
        const bool changed = shouts.equipped(actor) != shout;
        const bool done = magic::equip_voice(book, shouts, actor, shout);
        if (done && changed) {
            owner_.item_.send_equip_event(actor, shout, true);
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
        owner_.flush_magic_events();
    }
};


} // namespace skydot
