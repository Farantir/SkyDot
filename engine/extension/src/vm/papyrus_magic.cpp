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
        : owner_(owner), world_(world), effects(world, *this), book(world, effects) {
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
        }
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
        return book.equip(actor, spell, slot_of(hand));
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
    if (!actors_.is_actor(caster)) {
        out["reason"] = "no caster";
        return out;
    }
    if (actors_.is_dead(caster)) {
        out["reason"] = "dead";
        return out;
    }
    m->prime(caster);
    const double now = game_days();
    if (!m->book.ready(caster, spell, now)) {
        out["reason"] = "cooldown";
        return out;
    }
    double cost = m->spell_cost(spell, caster);
    if (s->casting_type() == magic::casting::concentration) {
        cost *= seconds > 0.0 ? seconds : 1.0; // per second
    }
    if (cost > 0.0 && actors_.get(caster, vm::av::k_magicka) < cost) {
        out["reason"] = "not enough magicka";
        out["cost"] = cost;
        return out;
    }
    std::uint32_t target = static_cast<std::uint32_t>(target_in);
    if (target == 0 || s->delivery() == 0) {
        target = caster;
    }
    // A recast of a running source (concentration) refreshes it and is not worth a log line.
    bool fresh = true;
    for (const auto& e : m->effects.effects_on(target)) {
        fresh = fresh && !(e.source.form == spell && e.caster == caster);
    }
    m->prime(target);
    if (fresh) {
        const std::string line = "magic: " + hex(caster) + " casts " + hex(spell) + " on " + hex(target) + " (cost " +
                                 number(std::round(cost * 100.0) / 100.0) + ")";
        emit_signal("trace", to_godot(line));
        godot::UtilityFunctions::print(to_godot(line));
    }
    if (cost > 0.0) {
        actors_.damage(caster, vm::av::k_magicka, cost);
    }
    m->book.note_used(caster, spell, now);
    const auto ids = m->land(caster, 0, spell, target);
    flush_magic_events();
    flush_deaths();
    out["ok"] = true;
    out["cost"] = cost;
    out["instances"] = static_cast<std::int64_t>(ids.size());
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
    const auto* base = lookup(m->world_.bases(), form);
    const auto* item = base != nullptr ? base->item() : nullptr;
    if (item != nullptr) {
        inputs = magic::inputs_of(item->effects());
        switch (item->category()) {
        case wfb::ItemCategory::ingredient: kind = magic::SourceKind::ingredient; break;
        case wfb::ItemCategory::scroll: kind = magic::SourceKind::scroll; break;
        default: break;
        }
    } else if (const auto* created = created_.find(form)) {
        poison = created->poison;
        std::int32_t index = 0;
        for (const auto& e : created->effects) {
            inputs.push_back({e.effect, e.magnitude, 0, e.duration, index++});
        }
    } else {
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
    static_cast<MagicState*>(magic_.get())->effects.on_death(ref);
    flush_magic_events();
}

void SkydotPapyrus::clear_magic() {
    if (!magic_) {
        return;
    }
    auto* m = static_cast<MagicState*>(magic_.get());
    m->effects.clear();
    m->book.clear();
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
    std::set<std::uint32_t> refs(m->book.refs().begin(), m->book.refs().end());
    for (const auto t : m->effects.targets()) {
        refs.insert(t);
    }
    refs.insert(k_player_ref);
    for (const auto ref : refs) {
        m->prime(ref);
    }
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
