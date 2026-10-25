// SPDX-License-Identifier: GPL-3.0-or-later
//
// Spell natives (vm/natives_magic.hpp). Source: the Creation Kit wiki's Spell script reference.
#include "vm/natives_magic.hpp"

#include "data/fb_search.hpp"
#include "magic/magic_rules.hpp"

#include <cmath>

namespace skydot::vm {

namespace {

const Value& arg(NativeCall& c, std::size_t i) {
    static const Value none;
    return i < c.args.size() ? c.args[i] : none;
}

NativeResult done(Value v) { return NativeResult{.value = std::move(v), .wait = -1.0}; }

namespace wfb = bethconv::pack::wfb;

} // namespace

void bind_spell_natives(Vm& vm, const wfb::World& world, MagicApi& api) {
    // Cast(akSource, akTarget = None): at no cost; with no target the spell goes on the source.
    vm.bind("Spell", "Cast", [&api](NativeCall& c) {
        const std::uint32_t source = arg(c, 0).form();
        if (source != 0) {
            api.cast(c.self.form(), source, 0, arg(c, 1).form());
        }
        return NativeResult{};
    });
    // RemoteCast(akSource, akBlameActor = None, akTarget = None).
    vm.bind("Spell", "RemoteCast", [&api](NativeCall& c) {
        const std::uint32_t source = arg(c, 0).form();
        if (source != 0) {
            api.cast(c.self.form(), source, arg(c, 1).form(), arg(c, 2).form());
        }
        return NativeResult{};
    });
    vm.bind("Spell", "GetMagickaCost", [&api](NativeCall& c) {
        return done(Value::integer(static_cast<std::int32_t>(api.spell_cost(c.self.form(), 0))));
    });
    vm.bind("Spell", "GetEffectiveMagickaCost", [&api](NativeCall& c) {
        return done(Value::integer(static_cast<std::int32_t>(std::floor(api.spell_cost(c.self.form(), arg(c, 0).form())))));
    });
    vm.bind("Spell", "GetCastTime", [&world](NativeCall& c) {
        const auto* s = lookup(world.spells(), c.self.form());
        return done(Value::floating(s != nullptr ? s->charge_time() : 0.0F));
    });
    vm.bind("Spell", "GetPerk", [&world](NativeCall& c) {
        const auto* s = lookup(world.spells(), c.self.form());
        return done(c.vm.object(s != nullptr ? s->half_cost_perk() : 0, "Perk"));
    });
    vm.bind("Spell", "IsHostile", [&world](NativeCall& c) {
        const auto* s = lookup(world.spells(), c.self.form());
        bool hostile = false;
        if (s != nullptr && s->effects() != nullptr) {
            for (const auto* e : *s->effects()) {
                const auto* m = lookup(world.magic_effects(), e->effect());
                hostile = hostile || (m != nullptr && (m->flags() & magic::mgef_flag::hostile) != 0);
            }
        }
        return done(Value::boolean(hostile));
    });
    vm.bind("Spell", "GetNumEffects", [&world](NativeCall& c) {
        const auto* s = lookup(world.spells(), c.self.form());
        return done(Value::integer(s != nullptr && s->effects() != nullptr ? static_cast<std::int32_t>(s->effects()->size()) : 0));
    });
    const auto nth = [&world](NativeCall& c) -> const wfb::EffectItem* {
        const auto* s = lookup(world.spells(), c.self.form());
        const auto index = static_cast<std::size_t>(std::max(0, arg(c, 0).i()));
        return s != nullptr && s->effects() != nullptr && index < s->effects()->size()
                   ? s->effects()->Get(static_cast<flatbuffers::uoffset_t>(index))
                   : nullptr;
    };
    vm.bind("Spell", "GetNthEffectMagicEffect", [nth](NativeCall& c) {
        const auto* e = nth(c);
        return done(c.vm.object(e != nullptr ? e->effect() : 0, "MagicEffect"));
    });
    vm.bind("Spell", "GetNthEffectMagnitude", [nth](NativeCall& c) {
        const auto* e = nth(c);
        return done(Value::floating(e != nullptr ? e->magnitude() : 0.0F));
    });
    vm.bind("Spell", "GetNthEffectArea", [nth](NativeCall& c) {
        const auto* e = nth(c);
        return done(Value::integer(e != nullptr ? static_cast<std::int32_t>(e->area()) : 0));
    });
    vm.bind("Spell", "GetNthEffectDuration", [nth](NativeCall& c) {
        const auto* e = nth(c);
        return done(Value::integer(e != nullptr ? static_cast<std::int32_t>(e->duration()) : 0));
    });
}

void bind_shout_natives(Vm& vm, const wfb::World& world) {
    const auto nth = [&world](NativeCall& c) -> const wfb::ShoutWord* {
        const auto* s = lookup(world.shouts(), c.self.form());
        const auto index = static_cast<std::size_t>(std::max(0, arg(c, 0).i()));
        return s != nullptr && s->words() != nullptr && index < s->words()->size()
                   ? s->words()->Get(static_cast<flatbuffers::uoffset_t>(index))
                   : nullptr;
    };
    vm.bind("Shout", "GetNthWordOfPower", [nth](NativeCall& c) {
        const auto* w = nth(c);
        return done(c.vm.object(w != nullptr ? w->word() : 0, "WordOfPower"));
    });
    vm.bind("Shout", "GetNthSpell", [nth](NativeCall& c) {
        const auto* w = nth(c);
        return done(c.vm.object(w != nullptr ? w->spell() : 0, "Spell"));
    });
    vm.bind("Shout", "GetNthRecoveryTime", [nth](NativeCall& c) {
        const auto* w = nth(c);
        return done(Value::floating(w != nullptr ? w->recovery() : 0.0F));
    });
}

} // namespace skydot::vm
