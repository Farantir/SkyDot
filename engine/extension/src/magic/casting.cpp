// SPDX-License-Identifier: GPL-3.0-or-later
#include "magic/casting.hpp"

#include "data/fb_search.hpp"
#include "vm/actor_states.hpp"

#include <algorithm>

namespace skydot::magic {

namespace {

/// The scroll item's data, null if `form` is no scroll.
const wfb::ItemData* scroll_item(const wfb::World& world, std::uint32_t form) {
    const auto* base = lookup(world.bases(), form);
    const auto* item = base != nullptr ? base->item() : nullptr;
    return item != nullptr && item->category() == wfb::ItemCategory::scroll ? item : nullptr;
}

/// The costliest entry of `effects` (cost as the editor sums it), null if none has a known MGEF.
const wfb::EffectItem* primary_entry(const wfb::World& world,
                                     const flatbuffers::Vector<flatbuffers::Offset<wfb::EffectItem>>* effects,
                                     std::uint32_t casting_type, const MagicSettings& s) {
    const wfb::EffectItem* best = nullptr;
    double best_cost = -1.0;
    if (effects == nullptr) {
        return nullptr;
    }
    for (const auto* e : *effects) {
        const auto* m = lookup(world.magic_effects(), e->effect());
        if (m == nullptr) {
            continue;
        }
        const double cost = effect_cost(static_cast<double>(m->base_cost()), static_cast<double>(e->magnitude()),
                                        static_cast<double>(e->duration()), casting_type, 0.0, s);
        if (cost > best_cost) {
            best_cost = cost;
            best = e;
        }
    }
    return best;
}

} // namespace

CastInfo cast_info(const wfb::World& world, std::uint32_t form, const MagicSettings& s) {
    CastInfo out;
    const auto* spell = lookup(world.spells(), form);
    const auto* scroll = spell == nullptr ? scroll_item(world, form) : nullptr;
    if (spell == nullptr && scroll == nullptr) {
        return out;
    }
    out.found = true;
    out.form = form;
    const flatbuffers::Vector<flatbuffers::Offset<wfb::EffectItem>>* effects = nullptr;
    if (spell != nullptr) {
        if (spell->name() != nullptr) {
            out.name = spell->name()->str();
        }
        out.type = spell->type();
        out.casting_type = spell->casting_type();
        out.delivery = spell->delivery();
        out.charge_time = spell->charge_time();
        out.cast_duration = spell->cast_duration();
        out.range = spell->range();
        effects = spell->effects();
    } else {
        const auto* base = lookup(world.bases(), form);
        out.name = base != nullptr && base->name() != nullptr ? base->name()->str() : std::string();
        effects = scroll->effects();
        out.casting_type = casting::fire_and_forget; // Skyrim scrolls are fire and forget
    }
    out.is_scroll = scroll != nullptr || out.casting_type == casting::scroll;
    if (out.is_scroll) {
        out.casting_type = casting::fire_and_forget;
    }
    out.effects = effects != nullptr ? effects->size() : 0;
    if (const auto* e = primary_entry(world, effects, out.casting_type, s)) {
        const auto* m = lookup(world.magic_effects(), e->effect());
        out.mgef = e->effect();
        out.magnitude = e->magnitude();
        out.duration = e->duration();
        out.area_feet = e->area();
        out.area_units = area_units(static_cast<double>(e->area()));
        out.projectile = m->projectile();
        out.explosion = m->explosion();
        out.casting_art = m->casting_art();
        out.hit_art = m->hit_effect_art();
        out.casting_light = m->casting_light();
        out.hostile = (m->flags() & mgef_flag::hostile) != 0;
        out.school = m->magic_skill();
        if (scroll != nullptr) {
            out.delivery = m->delivery(); // a scroll is cast the way its effect is delivered
        }
    }
    return out;
}

bool effects_of_castable(const wfb::World& world, std::uint32_t form, std::vector<EffectInput>& inputs,
                         SourceKind& kind) {
    if (const auto* s = lookup(world.spells(), form)) {
        inputs = inputs_of(s->effects());
        switch (s->type()) {
        case spell_type::ability: kind = SourceKind::ability; break;
        case spell_type::poison: kind = SourceKind::poison; break;
        default: kind = s->casting_type() == casting::scroll ? SourceKind::scroll : SourceKind::spell; break;
        }
        return true;
    }
    if (const auto* item = scroll_item(world, form)) {
        inputs = inputs_of(item->effects());
        kind = SourceKind::scroll;
        return true;
    }
    return false;
}

Payment pay_spell(const wfb::World& world, MagicHost& host, SpellBook& book, std::uint32_t caster,
                  std::uint32_t spell, double seconds, double game_days, const MagicSettings& s) {
    Payment out;
    const auto* sp = lookup(world.spells(), spell);
    if (sp == nullptr) {
        out.reason = "no such spell";
        return out;
    }
    if (!book.ready(caster, spell, game_days)) {
        out.reason = "cooldown";
        return out;
    }
    const int school = spell_school(world, *sp);
    const double skill = school >= 0 ? host.actor_value(caster, static_cast<std::uint32_t>(school)) : 0.0;
    double cost = spell_cost(world, *sp, skill, s);
    if (sp->casting_type() == casting::concentration) {
        cost *= seconds > 0.0 ? seconds : 1.0; // per second
    }
    out.cost = cost;
    if (cost > 0.0 && host.actor_value(caster, vm::av::k_magicka) < cost) {
        out.reason = "not enough magicka";
        return out;
    }
    if (cost > 0.0) {
        host.damage_value(caster, vm::av::k_magicka, cost, 0);
    }
    book.note_used(caster, spell, game_days);
    out.ok = true;
    return out;
}

Landing land_form(const wfb::World& world, ActiveEffects& effects, std::uint32_t caster, std::uint32_t form,
                  std::uint32_t target) {
    Landing out;
    std::vector<EffectInput> inputs;
    SourceKind kind = SourceKind::spell;
    if (!effects_of_castable(world, form, inputs, kind)) {
        out.reason = "no such spell";
        return out;
    }
    const auto* sp = lookup(world.spells(), form);
    if (target == 0 || (sp != nullptr && sp->delivery() == 0)) {
        target = caster;
    }
    out.instances = effects.apply(caster, target, {kind, form, 0}, inputs);
    out.ok = true;
    return out;
}

// ---- the hand ---------------------------------------------------------------------------------

const char* HandEvent::name() const noexcept {
    switch (kind) {
    case Kind::charged: return "charged";
    case Kind::cast: return "cast";
    case Kind::tick: return "tick";
    case Kind::stop: return "stop";
    case Kind::none: break;
    }
    return "";
}

void HandCast::press(const HandSpell& spell) {
    spell_ = spell;
    held_ = 0.0;
    announced_ = false;
    phase_ = spell.casting_type == casting::concentration ? Phase::concentrating : Phase::charging;
}

double HandCast::progress() const noexcept {
    if (phase_ != Phase::charging) {
        return 0.0;
    }
    return spell_.charge_time <= 0.0 ? 1.0 : std::min(1.0, held_ / spell_.charge_time);
}

HandEvent HandCast::step(double dt, const Payer& pay) {
    HandEvent out;
    if (dt <= 0.0) {
        return out;
    }
    switch (phase_) {
    case Phase::idle:
        break;
    case Phase::charging:
        held_ += dt;
        if (!announced_ && held_ >= spell_.charge_time) {
            announced_ = true;
            out.kind = HandEvent::Kind::charged;
            out.seconds = held_;
        }
        break;
    case Phase::concentrating:
        if (pay && !pay(dt)) {
            phase_ = Phase::idle;
            out.kind = HandEvent::Kind::stop;
            out.seconds = held_;
            break;
        }
        held_ += dt;
        out.kind = HandEvent::Kind::tick;
        out.seconds = dt;
        break;
    }
    return out;
}

HandEvent HandCast::release() {
    HandEvent out;
    out.seconds = held_;
    const Phase was = phase_;
    phase_ = Phase::idle;
    if (was == Phase::charging) {
        if (held_ >= spell_.charge_time) {
            out.kind = HandEvent::Kind::cast;
        }
    } else if (was == Phase::concentrating) {
        out.kind = HandEvent::Kind::stop;
    }
    held_ = 0.0;
    return out;
}

} // namespace skydot::magic
