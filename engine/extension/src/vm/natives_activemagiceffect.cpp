// SPDX-License-Identifier: GPL-3.0-or-later
//
// ActiveMagicEffect natives (vm/natives_magic.hpp): the script object of one running effect. Its
// handle is the effect's instance id; it answers also while OnEffectFinish is sent.
#include "vm/natives_magic.hpp"

namespace skydot::vm {

namespace {

NativeResult done(Value v) { return NativeResult{.value = std::move(v), .wait = -1.0}; }

} // namespace

void bind_activemagiceffect_natives(Vm& vm, MagicApi& api) {
    vm.bind("ActiveMagicEffect", "GetTargetActor", [&api](NativeCall& c) {
        const auto e = api.effect(c.self.form());
        return done(c.vm.object(e ? e->target : 0, "Actor"));
    });
    vm.bind("ActiveMagicEffect", "GetCasterActor", [&api](NativeCall& c) {
        const auto e = api.effect(c.self.form());
        return done(c.vm.object(e ? e->caster : 0, "Actor"));
    });
    vm.bind("ActiveMagicEffect", "GetBaseObject", [&api](NativeCall& c) {
        const auto e = api.effect(c.self.form());
        return done(c.vm.object(e ? e->mgef : 0, "MagicEffect"));
    });
    vm.bind("ActiveMagicEffect", "Dispel", [&api](NativeCall& c) {
        api.dispel_effect(c.self.form());
        return NativeResult{};
    });
    vm.bind("ActiveMagicEffect", "GetMagnitude", [&api](NativeCall& c) {
        const auto e = api.effect(c.self.form());
        return done(Value::floating(e ? static_cast<float>(e->magnitude) : 0.0F));
    });
    vm.bind("ActiveMagicEffect", "GetDuration", [&api](NativeCall& c) {
        const auto e = api.effect(c.self.form());
        return done(Value::integer(e ? static_cast<std::int32_t>(e->duration) : 0));
    });
    vm.bind("ActiveMagicEffect", "GetTimeElapsed", [&api](NativeCall& c) {
        const auto e = api.effect(c.self.form());
        return done(Value::integer(e ? static_cast<std::int32_t>(e->elapsed) : 0));
    });
}

} // namespace skydot::vm
