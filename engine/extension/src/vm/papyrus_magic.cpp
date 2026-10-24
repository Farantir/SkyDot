// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's magic: the glue between the rules of magic/ (docs/adr/0008), the VM's actor
// values, inventory and scripts, the pack's data and Godot. `MagicState` is the host the effects
// act through (MagicHost) and the API the natives call (MagicApi, vm/natives_magic.hpp).
//
// Events of effects (a start, a finish) are queued while the rules run and sent afterwards
// (`flush_magic_events`), because a script that answers one may dispel another effect.
#include "vm/papyrus.hpp"

#include "build/refs.hpp"
#include "conditions/functions.hpp"
#include "data/fb_search.hpp"
#include "data/leveled.hpp"
#include "magic/active_effects.hpp"
#include "magic/casting.hpp"
#include "magic/enchanted.hpp"
#include "magic/magic_save.hpp"
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

namespace {

namespace wfb = bethconv::pack::wfb;

String to_godot(const std::string& s) { return String::utf8(s.c_str(), static_cast<int>(s.size())); }

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%X", v);
    return buf;
}

std::string number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

constexpr int kind_value(magic::SourceKind k) { return static_cast<int>(k); }

} // namespace

struct SkydotPapyrus::MagicState final : SkydotPapyrus::MagicRuntime, magic::MagicHost, vm::MagicApi {
    MagicState(SkydotPapyrus& owner, const wfb::World& world)
        : owner_(owner), world_(world), effects(world, *this), book(world, effects), worn(world, effects) {
        book.set_base_source([this](std::uint32_t ref) {
            if (!owner_.actors_.is_actor(ref)) {
                return std::vector<std::uint32_t>{};
            }
            const std::uint32_t npc = owner_.dialogue_npc(ref);
            return npc != 0 ? magic::base_spells(world_, npc, ref, leveled::player_level())
                            : std::vector<std::uint32_t>{};
        });
    }

    SkydotPapyrus& owner_;
    const wfb::World& world_;
    magic::ActiveEffects effects;
    magic::SpellBook book;
    /// The constant effects of worn enchanted armor (never saved: derived when an actor is primed).
    magic::WornEnchantments worn;
    /// The attack button on each hand that holds a spell: (actor, hand) -> state.
    std::map<std::pair<std::uint32_t, int>, magic::HandCast> hands;

    struct Pending {
        bool started;
        magic::EffectView view;
    };
    std::vector<Pending> pending;
    bool flushing = false;
    /// Effects that have script objects: handle -> the last view, kept until the object is gone.
    std::map<std::uint32_t, magic::EffectView> snapshots;
    /// Handles to detach, with the updates left to wait (a script gets its OnEffectFinish first).
    std::vector<std::pair<std::uint32_t, int>> detach_later;
    bool abilities_primed = false;

    // ---- MagicHost
    double actor_value(std::uint32_t ref, std::uint32_t av) const override { return owner_.actors_.get(ref, av); }
    void damage_value(std::uint32_t ref, std::uint32_t av, double amount, std::uint32_t caster) override {
        owner_.actors_.damage(ref, av, amount);
        if (caster != 0 && av == vm::av::k_health) {
            if (owner_.actors_.get(ref, vm::av::k_health) <= 0.0 && owner_.actors_.killer_of(ref) == 0) {
                owner_.actors_.set_killer(ref, caster);
            }
            if (ref == k_player_ref) {
                owner_.last_damager_ = caster;
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
        owner_.resolve_owner(actor);
        std::vector<magic::WornEnchant> list;
        if (const vm::Equipment* eq = owner_.items_.find(actor)) {
            const auto add = [&](std::uint32_t form, std::uint32_t extra) {
                const vm::ItemInfo info = owner_.item_info_of(form);
                if (!info.known || (info.category != vm::ItemCategory::armor && info.category != vm::ItemCategory::clothing &&
                                    info.category != vm::ItemCategory::jewelry)) {
                    return;
                }
                if (const std::uint32_t ench = magic::enchantment_of(world_, form, owner_.items_.extras().find(extra))) {
                    list.push_back({form, extra, ench});
                }
            };
            for (const std::uint32_t form : eq->worn) {
                add(form, eq->worn_extra_of(form));
            }
            if (eq->left != 0 && vm::is_shield(owner_.item_info_of(eq->left))) {
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
        owner_.resolve_owner(actor);
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
        const bool scroll = !book.has_spell(actor, spell) && carried_scroll(actor, spell);
        return book.equip(actor, spell, slot_of(hand), scroll);
    }
    void unequip_spell(std::uint32_t actor, int hand) override { book.unequip(actor, slot_of(hand)); }
    std::uint32_t equipped_spell(std::uint32_t actor, int hand) override { return book.equipped(actor, slot_of(hand)); }
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

SkydotPapyrus::MagicState* SkydotPapyrus::magic_state() {
    if (magic_) {
        return static_cast<MagicState*>(magic_.get());
    }
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    if (root == nullptr || !vm_) {
        return nullptr;
    }
    magic_ = std::make_unique<MagicState>(*this, *root);
    return static_cast<MagicState*>(magic_.get());
}

void SkydotPapyrus::flush_magic_events() {
    auto* m = magic_state();
    if (m == nullptr || m->flushing) {
        return;
    }
    m->flushing = true;
    while (!m->pending.empty()) {
        auto batch = std::move(m->pending);
        m->pending.clear();
        for (const auto& event : batch) {
            const auto& v = event.view;
            emit_signal("magic_effect_changed", static_cast<std::int64_t>(v.target), static_cast<std::int64_t>(v.mgef),
                        static_cast<std::int64_t>(v.archetype), event.started, static_cast<std::int64_t>(v.id));
            const auto* mgef = lookup(m->world_.magic_effects(), v.mgef);
            const auto* scripts = mgef != nullptr ? mgef->scripts() : nullptr;
            if (scripts == nullptr || scripts->size() == 0) {
                continue;
            }
            // The script object of this effect: its handle is the instance id.
            m->snapshots[v.id] = v;
            const vm::Value target = vm_->object(v.target, "Actor");
            const vm::Value caster = vm_->object(v.caster, "Actor");
            if (event.started) {
                attach_scripts(v.id, script_list(scripts, false), 0);
                vm_->send_event(v.id, "OnEffectStart", {target, caster});
            } else {
                vm_->send_event(v.id, "OnEffectFinish", {target, caster});
                m->detach_later.emplace_back(v.id, 2);
            }
        }
    }
    m->flushing = false;
}

void SkydotPapyrus::tick_magic(double seconds) {
    auto* m = magic_state();
    if (m == nullptr) {
        return;
    }
    m->effects.tick(seconds);
    flush_magic_events();
    // Script objects of finished effects go once their OnEffectFinish had its updates.
    for (auto it = m->detach_later.begin(); it != m->detach_later.end();) {
        if (--it->second <= 0) {
            vm_->detach(it->first);
            m->snapshots.erase(it->first);
            it = m->detach_later.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- the game layer's API ------------------------------------------------------------------------

namespace {

/// The common start of the API's casts: ok false with the reason when `caster` cannot act.
bool caster_can_act(const vm::ActorStates& actors, std::uint32_t caster, Dictionary& out) {
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

} // namespace

Dictionary SkydotPapyrus::pay_spell(std::int64_t caster_in, std::int64_t spell_in, double seconds) {
    Dictionary out;
    out["ok"] = false;
    out["reason"] = String();
    out["cost"] = 0.0;
    auto* m = magic_state();
    const auto caster = static_cast<std::uint32_t>(caster_in);
    const auto spell = static_cast<std::uint32_t>(spell_in);
    if (m == nullptr || lookup(m->world_.spells(), spell) == nullptr) {
        out["reason"] = "no such spell";
        return out;
    }
    if (!caster_can_act(actors_, caster, out)) {
        return out;
    }
    m->prime(caster);
    const auto paid = magic::pay_spell(m->world_, *m, m->book, caster, spell, seconds, game_days(), m->settings());
    out["ok"] = paid.ok;
    out["reason"] = to_godot(paid.reason);
    out["cost"] = paid.cost;
    return out;
}

Dictionary SkydotPapyrus::land_spell(std::int64_t caster_in, std::int64_t form_in, std::int64_t target_in) {
    Dictionary out;
    out["ok"] = false;
    out["reason"] = String();
    out["instances"] = 0;
    auto* m = magic_state();
    const auto caster = static_cast<std::uint32_t>(caster_in);
    const auto form = static_cast<std::uint32_t>(form_in);
    if (m == nullptr || !magic::cast_info(m->world_, form).found) {
        out["reason"] = "no such spell";
        return out;
    }
    if (!caster_can_act(actors_, caster, out)) {
        return out;
    }
    const std::uint32_t target = target_in != 0 ? static_cast<std::uint32_t>(target_in) : caster;
    m->prime(caster);
    m->prime(target);
    const auto landed = magic::land_form(m->world_, m->effects, caster, form, target);
    flush_magic_events();
    flush_deaths();
    out["ok"] = landed.ok;
    out["reason"] = to_godot(landed.reason);
    out["instances"] = static_cast<std::int64_t>(landed.instances.size());
    return out;
}

Dictionary SkydotPapyrus::cast_spell(std::int64_t caster_in, std::int64_t spell_in, std::int64_t target_in,
                                     double seconds) {
    Dictionary out;
    out["ok"] = false;
    out["reason"] = String();
    out["cost"] = 0.0;
    out["instances"] = 0;
    auto* m = magic_state();
    const auto caster = static_cast<std::uint32_t>(caster_in);
    const auto spell = static_cast<std::uint32_t>(spell_in);
    const auto* s = m != nullptr ? lookup(m->world_.spells(), spell) : nullptr;
    if (s == nullptr) {
        out["reason"] = "no such spell";
        return out;
    }
    if (!caster_can_act(actors_, caster, out)) {
        return out;
    }
    m->prime(caster);
    std::uint32_t target = static_cast<std::uint32_t>(target_in);
    if (target == 0 || s->delivery() == 0) {
        target = caster;
    }
    // A recast of a running source (concentration) refreshes it and is not worth a log line.
    bool fresh = true;
    for (const auto& e : m->effects.effects_on(target)) {
        fresh = fresh && !(e.source.form == spell && e.caster == caster);
    }
    const auto paid = magic::pay_spell(m->world_, *m, m->book, caster, spell, seconds, game_days(), m->settings());
    out["reason"] = to_godot(paid.reason);
    out["cost"] = paid.cost;
    if (!paid.ok) {
        return out;
    }
    m->prime(target);
    if (fresh) {
        const std::string line = "magic: " + hex(caster) + " casts " + hex(spell) + " on " + hex(target) + " (cost " +
                                 number(std::round(paid.cost * 100.0) / 100.0) + ")";
        emit_signal("trace", to_godot(line));
        godot::UtilityFunctions::print(to_godot(line));
    }
    const auto landed = magic::land_form(m->world_, m->effects, caster, spell, target);
    flush_magic_events();
    flush_deaths();
    out["ok"] = true;
    out["instances"] = static_cast<std::int64_t>(landed.instances.size());
    return out;
}

Dictionary SkydotPapyrus::cast_info(std::int64_t form_in) {
    Dictionary out;
    out["found"] = false;
    auto* m = magic_state();
    if (m == nullptr) {
        return out;
    }
    const auto info = magic::cast_info(m->world_, static_cast<std::uint32_t>(form_in), m->settings());
    if (!info.found) {
        return out;
    }
    const auto id = [](std::uint32_t v) { return static_cast<std::int64_t>(v); };
    out["found"] = true;
    out["form"] = form_in;
    out["name"] = to_godot(info.name);
    out["is_scroll"] = info.is_scroll;
    out["type"] = id(info.type);
    out["casting_type"] = id(info.casting_type);
    out["delivery"] = id(info.delivery);
    out["charge_time"] = static_cast<double>(info.charge_time);
    out["cast_duration"] = static_cast<double>(info.cast_duration);
    out["range"] = static_cast<double>(info.range);
    out["effects"] = static_cast<std::int64_t>(info.effects);
    out["mgef"] = id(info.mgef);
    out["magnitude"] = static_cast<double>(info.magnitude);
    out["duration"] = id(info.duration);
    out["area_feet"] = id(info.area_feet);
    out["area_units"] = info.area_units;
    out["projectile"] = id(info.projectile);
    out["explosion"] = id(info.explosion);
    out["casting_art"] = id(info.casting_art);
    out["hit_art"] = id(info.hit_art);
    out["casting_light"] = id(info.casting_light);
    out["hostile"] = info.hostile;
    out["school"] = info.school;
    return out;
}

Dictionary SkydotPapyrus::cast_scroll(std::int64_t caster_in, std::int64_t scroll_in, std::int64_t target_in) {
    Dictionary out;
    out["ok"] = false;
    out["reason"] = String();
    out["instances"] = 0;
    auto* m = magic_state();
    const auto caster = static_cast<std::uint32_t>(caster_in);
    const auto scroll = static_cast<std::uint32_t>(scroll_in);
    const auto info = m != nullptr ? magic::cast_info(m->world_, scroll) : magic::CastInfo{};
    if (!info.found || !info.is_scroll) {
        out["reason"] = "no such scroll";
        return out;
    }
    if (!caster_can_act(actors_, caster, out)) {
        return out;
    }
    const bool item = lookup(m->world_.spells(), scroll) == nullptr; // a SCRL; a SPEL scroll is not carried
    if (item && !m->carried_scroll(caster, scroll)) {
        out["reason"] = "not carried";
        return out;
    }
    const std::uint32_t target = target_in != 0 ? static_cast<std::uint32_t>(target_in) : caster;
    m->prime(caster);
    m->prime(target);
    const std::string line = "magic: " + hex(caster) + " reads " + hex(scroll) + " on " + hex(target);
    emit_signal("trace", to_godot(line));
    godot::UtilityFunctions::print(to_godot(line));
    const auto landed = magic::land_form(m->world_, m->effects, caster, scroll, target);
    if (item) {
        (void)remove_item(caster, scroll, 1);
        if (vm_->inventory().count(caster, scroll) <= 0) {
            for (const int hand : {0, 1}) {
                if (m->equipped_spell(caster, hand) == scroll) {
                    m->unequip_spell(caster, hand);
                }
            }
        }
    }
    flush_magic_events();
    flush_deaths();
    out["ok"] = landed.ok;
    out["reason"] = to_godot(landed.reason);
    out["instances"] = static_cast<std::int64_t>(landed.instances.size());
    return out;
}

double SkydotPapyrus::spell_cost(std::int64_t caster, std::int64_t spell) {
    auto* m = magic_state();
    return m != nullptr ? m->spell_cost(static_cast<std::uint32_t>(spell), static_cast<std::uint32_t>(caster)) : 0.0;
}

void SkydotPapyrus::spend_magicka(std::int64_t caster, double amount) {
    if (amount > 0.0) {
        actors_.damage(static_cast<std::uint32_t>(caster), vm::av::k_magicka, amount);
    }
}

Dictionary SkydotPapyrus::apply_item_effects(std::int64_t actor_in, std::int64_t form_in, std::int64_t extra) {
    Dictionary out;
    out["ok"] = false;
    out["reason"] = String();
    out["effects"] = 0;
    auto* m = magic_state();
    const auto actor = static_cast<std::uint32_t>(actor_in);
    const auto form = static_cast<std::uint32_t>(form_in);
    if (m == nullptr || !actors_.is_actor(actor)) {
        out["reason"] = "no actor";
        return out;
    }
    std::vector<magic::EffectInput> inputs;
    magic::SourceKind kind = magic::SourceKind::potion;
    bool poison = false;
    if (!m->item_effects(form, inputs, kind, poison)) {
        out["reason"] = "not an item";
        return out;
    }
    if (kind == magic::SourceKind::ingredient && inputs.size() > 1) {
        inputs.resize(1); // eating an ingredient gives its first effect
    }
    if (poison) {
        out["reason"] = "poison";
        return out;
    }
    if (inputs.empty()) {
        out["reason"] = "no effects";
        return out;
    }
    m->prime(actor);
    const auto ids = m->effects.apply(actor, actor, {kind, form, static_cast<std::uint32_t>(std::max<std::int64_t>(extra, 0))}, inputs);
    flush_magic_events();
    flush_deaths();
    out["ok"] = true;
    out["effects"] = static_cast<std::int64_t>(ids.size());
    return out;
}

godot::PackedInt64Array SkydotPapyrus::spells_of(std::int64_t actor) {
    godot::PackedInt64Array out;
    auto* m = magic_state();
    if (m != nullptr && actors_.is_actor(static_cast<std::uint32_t>(actor))) {
        for (const auto s : m->book.spells_of(static_cast<std::uint32_t>(actor))) {
            out.push_back(s);
        }
    }
    return out;
}

bool SkydotPapyrus::add_spell(std::int64_t actor, std::int64_t spell) {
    auto* m = magic_state();
    return m != nullptr && m->add_spell(static_cast<std::uint32_t>(actor), static_cast<std::uint32_t>(spell), false);
}

bool SkydotPapyrus::remove_spell(std::int64_t actor, std::int64_t spell) {
    auto* m = magic_state();
    return m != nullptr && m->remove_spell(static_cast<std::uint32_t>(actor), static_cast<std::uint32_t>(spell));
}

bool SkydotPapyrus::has_spell(std::int64_t actor, std::int64_t spell) {
    auto* m = magic_state();
    return m != nullptr && m->has_spell(static_cast<std::uint32_t>(actor), static_cast<std::uint32_t>(spell));
}

bool SkydotPapyrus::equip_spell(std::int64_t actor, std::int64_t spell, std::int64_t hand) {
    auto* m = magic_state();
    return m != nullptr && m->equip_spell(static_cast<std::uint32_t>(actor), static_cast<std::uint32_t>(spell),
                                          static_cast<int>(hand));
}

std::int64_t SkydotPapyrus::equipped_spell(std::int64_t actor, std::int64_t hand) {
    auto* m = magic_state();
    return m != nullptr ? static_cast<std::int64_t>(m->equipped_spell(static_cast<std::uint32_t>(actor), static_cast<int>(hand))) : 0;
}

double SkydotPapyrus::power_cooldown(std::int64_t actor, std::int64_t spell) {
    auto* m = magic_state();
    return m != nullptr ? m->book.cooldown_left(static_cast<std::uint32_t>(actor), static_cast<std::uint32_t>(spell), game_days()) : 0.0;
}

Array SkydotPapyrus::active_effects(std::int64_t actor_in) {
    Array out;
    auto* m = magic_state();
    const auto actor = static_cast<std::uint32_t>(actor_in);
    if (m == nullptr || !actors_.is_actor(actor)) {
        return out;
    }
    m->prime(actor);
    for (const auto& e : m->effects.effects_on(actor)) {
        Dictionary d;
        const auto* mgef = lookup(m->world_.magic_effects(), e.mgef);
        d["instance"] = static_cast<std::int64_t>(e.id);
        d["mgef"] = static_cast<std::int64_t>(e.mgef);
        d["name"] = mgef != nullptr && mgef->name() != nullptr ? to_godot(mgef->name()->str()) : String();
        d["magnitude"] = e.magnitude;
        d["duration"] = e.duration;
        d["elapsed"] = e.elapsed;
        d["remaining"] = e.permanent ? -1.0 : std::max(0.0, e.duration - e.elapsed);
        d["source"] = static_cast<std::int64_t>(e.source.form);
        d["source_kind"] = kind_value(e.source.kind);
        d["caster"] = static_cast<std::int64_t>(e.caster);
        d["archetype"] = static_cast<std::int64_t>(e.archetype);
        d["active"] = e.active;
        out.push_back(d);
    }
    return out;
}

bool SkydotPapyrus::dispel_spell(std::int64_t actor, std::int64_t spell) {
    auto* m = magic_state();
    return m != nullptr && m->dispel_spell(static_cast<std::uint32_t>(actor), static_cast<std::uint32_t>(spell));
}

void SkydotPapyrus::dispel_all_spells(std::int64_t actor) {
    if (auto* m = magic_state()) {
        m->dispel_all_spells(static_cast<std::uint32_t>(actor));
    }
}

void SkydotPapyrus::prepare_actor_magic(std::int64_t actor) {
    if (auto* m = magic_state()) {
        m->prime(static_cast<std::uint32_t>(actor));
        flush_magic_events();
    }
}

Dictionary SkydotPapyrus::get_unhandled_archetypes() const {
    Dictionary out;
    if (magic_) {
        for (const auto& [a, n] : static_cast<MagicState*>(magic_.get())->effects.unhandled()) {
            out[static_cast<std::int64_t>(a)] = static_cast<std::int64_t>(n);
        }
    }
    return out;
}

void SkydotPapyrus::magic_actor_died(std::uint32_t ref) {
    if (!magic_) {
        return;
    }
    auto* m = static_cast<MagicState*>(magic_.get());
    m->effects.on_death(ref);
    m->worn.forget(ref);
    std::erase_if(m->hands, [ref](const auto& h) { return h.first.first == ref; });
    flush_magic_events();
}

void SkydotPapyrus::clear_magic() {
    if (!magic_) {
        return;
    }
    auto* m = static_cast<MagicState*>(magic_.get());
    m->effects.clear();
    m->book.clear();
    m->worn.clear();
    m->hands.clear();
    m->pending.clear();
    m->snapshots.clear();
    m->detach_later.clear();
}

std::vector<std::uint8_t> SkydotPapyrus::magic_section() const {
    if (!magic_) {
        return {};
    }
    const auto* m = static_cast<const MagicState*>(magic_.get());
    return magic::write_magc(m->effects, m->book);
}

bool SkydotPapyrus::magic_decode(save::ByteReader r, std::vector<std::uint8_t>& effects,
                                 std::vector<std::uint8_t>& book) {
    const auto* root = world_.is_valid() ? world_->data().root() : nullptr;
    magic::MagcParts parts;
    if (root == nullptr || !magic::read_magc(r, parts) || !magic::check_magc(*root, parts)) {
        return false;
    }
    effects = std::move(parts.effects);
    book = std::move(parts.book);
    return true;
}

void SkydotPapyrus::magic_adopt(bool has, const std::vector<std::uint8_t>& effects,
                                const std::vector<std::uint8_t>& book) {
    auto* m = magic_state();
    if (m == nullptr) {
        return;
    }
    m->effects.clear();
    m->book.clear();
    m->worn.clear();
    m->hands.clear();
    m->pending.clear();
    m->snapshots.clear();
    m->detach_later.clear();
    if (has) {
        // The actor values were replaced without their temporary part: the effects hold it again.
        (void)magic::SpellBook::load(save::ByteReader(book), m->book);
        (void)m->effects.load(save::ByteReader(effects));
    }
    // Abilities are not saved: start them again for whoever is known.
    m->book.reset_abilities();
    std::vector<std::uint32_t> wearers;
    for (const auto& [owner, equipment] : items_.equipments()) {
        if (!equipment.worn.empty() || equipment.left != 0) {
            wearers.push_back(owner);
        }
    }
    const auto refs = magic::actors_to_rederive(m->book, m->effects, wearers);
    for (const auto ref : refs) {
        m->prime(ref);
    }
}

// ---- worn enchantments, poisons, enchanted weapons ---------------------------------------------------

void SkydotPapyrus::sync_worn_magic(std::uint32_t owner) {
    if (!magic_) {
        return; // priming derives them when magic first runs
    }
    static_cast<MagicState*>(magic_.get())->sync_worn(owner);
    flush_magic_events();
}

void SkydotPapyrus::move_unit_extra(std::uint32_t owner, std::uint32_t form, std::uint32_t from, std::uint32_t to) {
    if (from == to || owner == 0) {
        return;
    }
    auto& inventory = vm_->inventory();
    if (inventory.remove_extra(owner, form, from, 1) > 0) {
        inventory.add(owner, form, 1, to);
    }
    if (items_.find(owner) == nullptr) {
        return;
    }
    auto& equipment = items_.equipment(owner);
    if (equipment.right == form && equipment.right_extra == from) {
        equipment.right_extra = to;
    } else if (equipment.left == form && equipment.left_extra == from) {
        equipment.left_extra = to;
    } else if (equipment.ammo == form && equipment.ammo_extra == from) {
        equipment.ammo_extra = to;
    } else if (equipment.worn.contains(form) && equipment.worn_extra_of(form) == from) {
        if (to == 0) {
            equipment.worn_extra.erase(form);
        } else {
            equipment.worn_extra[form] = to;
        }
    }
}

Dictionary SkydotPapyrus::hit_magic(std::uint32_t attacker, std::uint32_t target, std::uint32_t weapon,
                                    std::uint32_t extra, bool target_alive) {
    Dictionary out;
    out["poison"] = 0;
    out["enchantment"] = 0;
    out["fired"] = false;
    out["charge"] = 0.0;
    out["charge_max"] = 0.0;
    auto* m = magic_state();
    if (m == nullptr || weapon == 0) {
        return out;
    }
    m->prime(attacker);
    m->prime(target);
    magic::HitMagicInput in;
    in.attacker = attacker;
    in.target = target;
    in.weapon = weapon;
    in.extra = extra;
    in.attacker_is_player = attacker == k_player_ref;
    in.target_alive = target_alive;
    const auto poison_inputs = [m](std::uint32_t poison, std::vector<magic::EffectInput>& inputs) {
        magic::SourceKind kind = magic::SourceKind::potion;
        bool is_poison = false;
        return m->item_effects(poison, inputs, kind, is_poison);
    };
    const auto result = magic::hit_magic(m->world_, m->effects, items_.extras(), in, poison_inputs, m->settings());
    if (result.new_extra != extra) {
        move_unit_extra(attacker, weapon, extra, result.new_extra);
    }
    flush_magic_events();
    out["poison"] = static_cast<std::int64_t>(result.poison);
    out["enchantment"] = static_cast<std::int64_t>(result.enchantment);
    out["fired"] = result.fired;
    out["charge"] = static_cast<double>(result.charge);
    out["charge_max"] = static_cast<double>(result.max);
    if (result.enchantment != 0 && attacker == k_player_ref) {
        const std::string line = "magic: " + hex(weapon) + " enchantment " + hex(result.enchantment) +
                                 (result.fired ? " fires" : " has no charge") + " (charge " +
                                 number(static_cast<double>(result.charge)) + "/" +
                                 number(static_cast<double>(result.max)) + ")";
        emit_signal("trace", to_godot(line));
        godot::UtilityFunctions::print(to_godot(line));
    }
    return out;
}

Dictionary SkydotPapyrus::poison_weapon(std::uint32_t actor, std::uint32_t form, std::int64_t extra) {
    Dictionary out;
    out["ok"] = false;
    out["reason"] = String();
    out["weapon"] = 0;
    out["doses"] = 0;
    auto* m = magic_state();
    if (m == nullptr || !actors_.is_actor(actor)) {
        out["reason"] = "no actor";
        return out;
    }
    std::vector<magic::EffectInput> inputs;
    magic::SourceKind kind = magic::SourceKind::potion;
    bool poison = false;
    if (!m->item_effects(form, inputs, kind, poison) || !poison) {
        out["reason"] = "not a poison";
        return out;
    }
    resolve_owner(actor);
    const std::uint32_t poison_extra = resolve_extra(actor, form, extra);
    if (vm_->inventory().count_extra(actor, form, poison_extra) <= 0) {
        out["reason"] = "not carried";
        return out;
    }
    const vm::Equipment* equipment = items_.find(actor);
    const std::uint32_t weapon = equipment != nullptr ? equipment->right : 0;
    const vm::ItemInfo info = item_info_of(weapon);
    // UESP: a poison goes on the weapon in the right hand, a bow included; a staff or fists have no blade.
    if (weapon == 0 || !info.known || info.category != vm::ItemCategory::weapon || info.weapon_type == 0 ||
        info.weapon_type == 8) {
        out["reason"] = "no weapon";
        return out;
    }
    const std::uint32_t weapon_extra = equipment->right_extra;
    vm::ItemExtra changed;
    if (const auto* now = items_.extras().find(weapon_extra)) {
        changed = *now;
    }
    if (!magic::can_poison(changed)) {
        out["reason"] = "already poisoned";
        return out;
    }
    (void)remove_item_extra(actor, form, poison_extra, 1);
    const std::uint32_t after = items_.extras().intern(magic::poisoned(changed, form, magic::k_placeholder_poison_doses));
    move_unit_extra(actor, weapon, weapon_extra, after);
    const std::string line = "magic: poison " + hex(form) + " put on " + hex(weapon) + " of " + hex(actor);
    emit_signal("trace", to_godot(line));
    godot::UtilityFunctions::print(to_godot(line));
    out["ok"] = true;
    out["weapon"] = static_cast<std::int64_t>(weapon);
    out["doses"] = static_cast<std::int64_t>(magic::k_placeholder_poison_doses);
    return out;
}

Dictionary SkydotPapyrus::apply_poison(std::int64_t actor, std::int64_t poison) {
    return poison_weapon(static_cast<std::uint32_t>(actor), static_cast<std::uint32_t>(poison), -1);
}

Dictionary SkydotPapyrus::weapon_charge(std::int64_t actor_in, std::int64_t form_in, std::int64_t extra) {
    Dictionary out;
    out["enchantment"] = 0;
    out["charge"] = 0.0;
    out["max"] = 0.0;
    out["cost"] = 0.0;
    auto* m = magic_state();
    if (m == nullptr) {
        return out;
    }
    const auto actor = static_cast<std::uint32_t>(actor_in);
    const auto form = static_cast<std::uint32_t>(form_in);
    resolve_owner(actor);
    std::uint32_t unit = 0;
    const vm::Equipment* equipment = items_.find(actor);
    if (extra >= 0) {
        unit = static_cast<std::uint32_t>(extra);
    } else if (equipment != nullptr && equipment->right == form) {
        unit = equipment->right_extra;
    } else {
        unit = resolve_extra(actor, form, -1);
    }
    const auto info = magic::weapon_charge(m->world_, form, items_.extras().find(unit), m->settings());
    out["enchantment"] = static_cast<std::int64_t>(info.enchantment);
    out["charge"] = static_cast<double>(info.charge);
    out["max"] = static_cast<double>(info.max);
    out["cost"] = info.cost;
    return out;
}

// ---- the hands -------------------------------------------------------------------------------------

Dictionary SkydotPapyrus::hand_press(std::int64_t actor_in, std::int64_t hand_in) {
    Dictionary out;
    out["ok"] = false;
    out["reason"] = String();
    out["spell"] = 0;
    out["casting_type"] = 0;
    out["charge_time"] = 0.0;
    out["is_scroll"] = false;
    auto* m = magic_state();
    const auto actor = static_cast<std::uint32_t>(actor_in);
    const int hand = static_cast<int>(hand_in);
    if (m == nullptr) {
        out["reason"] = "no spell";
        return out;
    }
    const std::uint32_t form = m->equipped_spell(actor, hand);
    const auto info = magic::cast_info(m->world_, form, m->settings());
    if (form == 0 || !info.found) {
        out["reason"] = "no spell";
        return out;
    }
    if (!caster_can_act(actors_, actor, out)) {
        return out;
    }
    m->prime(actor);
    out["spell"] = static_cast<std::int64_t>(form);
    out["casting_type"] = static_cast<std::int64_t>(info.casting_type);
    out["charge_time"] = static_cast<double>(info.charge_time);
    out["is_scroll"] = info.is_scroll;
    if (info.is_scroll) {
        if (lookup(m->world_.spells(), form) == nullptr && !m->carried_scroll(actor, form)) {
            m->unequip_spell(actor, hand);
            out["reason"] = "not carried";
            return out;
        }
    } else {
        if (!m->book.ready(actor, form, game_days())) {
            out["reason"] = "cooldown";
            return out;
        }
        const double magicka = actors_.get(actor, vm::av::k_magicka);
        const double cost = m->spell_cost(form, actor);
        const bool short_of = info.casting_type == magic::casting::concentration ? magicka <= 0.0
                                                                                : (cost > 0.0 && magicka < cost);
        if (short_of) {
            out["reason"] = "not enough magicka";
            return out;
        }
    }
    m->hands[{actor, hand}].press({info.casting_type, static_cast<double>(info.charge_time)});
    out["ok"] = true;
    return out;
}

Dictionary SkydotPapyrus::hand_step(std::int64_t actor_in, std::int64_t hand_in, double dt) {
    Dictionary out;
    out["event"] = String();
    out["seconds"] = 0.0;
    out["progress"] = 0.0;
    out["cost"] = 0.0;
    out["spell"] = 0;
    out["hand"] = hand_in;
    auto* m = magic_state();
    const auto actor = static_cast<std::uint32_t>(actor_in);
    const int hand = static_cast<int>(hand_in);
    const auto it = m != nullptr ? m->hands.find({actor, hand}) : decltype(m->hands.end()){};
    if (m == nullptr || it == m->hands.end()) {
        return out;
    }
    magic::HandCast& cast = it->second;
    const std::uint32_t form = m->equipped_spell(actor, hand);
    out["spell"] = static_cast<std::int64_t>(form);
    if (form == 0 || actors_.is_dead(actor)) {
        const bool was_held = cast.phase() == magic::HandCast::Phase::concentrating;
        cast.cancel();
        out["event"] = was_held ? "stop" : "";
        return out;
    }
    double paid = 0.0;
    const auto event = cast.step(dt, [&](double seconds) {
        const auto p = magic::pay_spell(m->world_, *m, m->book, actor, form, seconds, game_days(), m->settings());
        paid += p.ok ? p.cost : 0.0;
        return p.ok;
    });
    out["event"] = event.name();
    out["seconds"] = event.seconds;
    out["progress"] = cast.progress();
    out["cost"] = paid;
    return out;
}

Dictionary SkydotPapyrus::hand_release(std::int64_t actor_in, std::int64_t hand_in) {
    Dictionary out;
    out["event"] = String();
    out["seconds"] = 0.0;
    out["spell"] = 0;
    out["is_scroll"] = false;
    out["hand"] = hand_in;
    auto* m = magic_state();
    const auto actor = static_cast<std::uint32_t>(actor_in);
    const int hand = static_cast<int>(hand_in);
    const auto it = m != nullptr ? m->hands.find({actor, hand}) : decltype(m->hands.end()){};
    if (m == nullptr || it == m->hands.end()) {
        return out;
    }
    const auto event = it->second.release();
    const std::uint32_t form = m->equipped_spell(actor, hand);
    out["event"] = event.name();
    out["seconds"] = event.seconds;
    out["spell"] = static_cast<std::int64_t>(form);
    out["is_scroll"] = magic::cast_info(m->world_, form).is_scroll;
    return out;
}

void SkydotPapyrus::bind_magic_natives() {
    // The natives need the world; a VM without one has no spells to cast (setup binds again).
    auto* m = magic_state();
    if (m == nullptr) {
        return;
    }
    vm::bind_spell_natives(*vm_, m->world_, *m);
    vm::bind_magiceffect_natives(*vm_, m->world_);
    vm::bind_activemagiceffect_natives(*vm_, *m);
    vm::bind_actor_magic_natives(*vm_, *m);
}

} // namespace skydot
