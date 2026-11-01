// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus's spell casting for the game layer: paying and landing spells, scrolls and item
// effects, the spell book and the active effects (rules in magic/casting.hpp; state in MagicState).
#include "vm/papyrus_magic_state.hpp"

namespace skydot {

// ---- the game layer's API ------------------------------------------------------------------------


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
    if (paid.ok && seconds <= 0.0) {
        send_spell_cast(caster, spell); // a concentration spell pays every frame: one event per cast
    }
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
    if (fresh) {
        send_spell_cast(caster, spell);
    }
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
    out["is_item"] = lookup(m->world_.spells(), static_cast<std::uint32_t>(form_in)) == nullptr; // a SCRL item, not a SPEL
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
    out["recover"] = info.recover;
    out["archetype"] = static_cast<std::int64_t>(info.archetype);
    out["actor_value"] = info.actor_value;
    out["school"] = info.school;
    out["base_cost"] = info.base_cost;
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
    send_spell_cast(caster, scroll);
    if (item) {
        (void)item_.remove_item(caster, scroll, 1);
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

void SkydotPapyrus::set_blocking(std::int64_t actor, bool blocking) {
    if (blocking) {
        blocking_.insert(static_cast<std::uint32_t>(actor));
    } else {
        blocking_.erase(static_cast<std::uint32_t>(actor));
    }
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

void SkydotPapyrus::unequip_spell(std::int64_t actor, std::int64_t hand) {
    if (auto* m = magic_state()) {
        m->unequip_spell(static_cast<std::uint32_t>(actor), static_cast<int>(hand));
    }
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
        d["associated"] = static_cast<std::int64_t>(e.associated);
        d["summoned"] = static_cast<std::int64_t>(e.summoned);
        d["area"] = e.area;
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
        for (const auto& [a, n] : magic_.get()->effects.unhandled()) {
            out[static_cast<std::int64_t>(a)] = static_cast<std::int64_t>(n);
        }
    }
    return out;
}


void SkydotPapyrus::send_spell_cast(std::uint32_t caster, std::uint32_t spell) {
    dispatch(caster, "OnSpellCast", {vm_->object(spell, "Form")});
}

} // namespace skydot
