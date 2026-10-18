// SPDX-License-Identifier: GPL-3.0-or-later
//
// MagicEffect natives (vm/natives_magic.hpp): what the MGEF record says. Source: the Creation Kit
// wiki's MagicEffect script reference.
#include "vm/natives_magic.hpp"

#include "data/fb_search.hpp"
#include "vm/actor_values.hpp"

namespace skydot::vm {

namespace {

const Value& arg(NativeCall& c, std::size_t i) {
    static const Value none;
    return i < c.args.size() ? c.args[i] : none;
}

NativeResult done(Value v) { return NativeResult{.value = std::move(v), .wait = -1.0}; }

namespace wfb = bethconv::pack::wfb;

std::string av_name(std::int32_t av) {
    if (av < 0 || static_cast<std::size_t>(av) >= av::k_count) {
        return {};
    }
    return std::string(av::k_names[static_cast<std::size_t>(av)]);
}

} // namespace

void bind_magiceffect_natives(Vm& vm, const wfb::World& world) {
    const auto effect = [&world](NativeCall& c) { return lookup(world.magic_effects(), c.self.form()); };
    const auto integer = [&](const char* fn, auto read) {
        vm.bind("MagicEffect", fn, [effect, read](NativeCall& c) {
            const auto* m = effect(c);
            return done(Value::integer(m != nullptr ? static_cast<std::int32_t>(read(*m)) : 0));
        });
    };
    const auto real = [&](const char* fn, auto read) {
        vm.bind("MagicEffect", fn, [effect, read](NativeCall& c) {
            const auto* m = effect(c);
            return done(Value::floating(m != nullptr ? static_cast<float>(read(*m)) : 0.0F));
        });
    };
    const auto form = [&](const char* fn, const char* cls, auto read) {
        vm.bind("MagicEffect", fn, [effect, read, cls](NativeCall& c) {
            const auto* m = effect(c);
            return done(c.vm.object(m != nullptr ? static_cast<std::uint32_t>(read(*m)) : 0, cls));
        });
    };
    integer("GetCastingType", [](const wfb::MagicEffect& m) { return m.casting_type(); });
    integer("GetDeliveryType", [](const wfb::MagicEffect& m) { return m.delivery(); });
    integer("GetSkillLevel", [](const wfb::MagicEffect& m) { return m.skill_level(); });
    real("GetBaseCost", [](const wfb::MagicEffect& m) { return m.base_cost(); });
    real("GetCastTime", [](const wfb::MagicEffect& m) { return m.casting_time(); });
    real("GetSkillUsageMult", [](const wfb::MagicEffect& m) { return m.skill_usage_multiplier(); });
    form("GetPerk", "Perk", [](const wfb::MagicEffect& m) { return m.perk_to_apply(); });
    form("GetProjectile", "Projectile", [](const wfb::MagicEffect& m) { return m.projectile(); });
    form("GetExplosion", "Explosion", [](const wfb::MagicEffect& m) { return m.explosion(); });
    form("GetLight", "Light", [](const wfb::MagicEffect& m) { return m.casting_light(); });
    form("GetCastingArt", "Art", [](const wfb::MagicEffect& m) { return m.casting_art(); });
    form("GetHitEffectArt", "Art", [](const wfb::MagicEffect& m) { return m.hit_effect_art(); });
    form("GetEnchantArt", "Art", [](const wfb::MagicEffect& m) { return m.enchant_art(); });
    form("GetEquipAbility", "Spell", [](const wfb::MagicEffect& m) { return m.equip_ability(); });
    form("GetImpactDataSet", "ImpactDataSet", [](const wfb::MagicEffect& m) { return m.impact_data(); });
    form("GetHitShader", "EffectShader", [](const wfb::MagicEffect& m) { return m.hit_shader(); });
    form("GetEnchantShader", "EffectShader", [](const wfb::MagicEffect& m) { return m.enchant_shader(); });
    // The skill the effect is cast with (an actor value name), and the actor value that resists it.
    vm.bind("MagicEffect", "GetAssociatedSkill", [effect](NativeCall& c) {
        const auto* m = effect(c);
        return done(Value::string(m != nullptr ? av_name(m->magic_skill()) : std::string{}));
    });
    vm.bind("MagicEffect", "GetResistance", [effect](NativeCall& c) {
        const auto* m = effect(c);
        return done(Value::string(m != nullptr ? av_name(m->resist_value()) : std::string{}));
    });
    vm.bind("MagicEffect", "GetNumCounterEffects", [effect](NativeCall& c) {
        const auto* m = effect(c);
        return done(Value::integer(m != nullptr && m->counter_effects() != nullptr
                                       ? static_cast<std::int32_t>(m->counter_effects()->size())
                                       : 0));
    });
    vm.bind("MagicEffect", "GetNthCounterEffect", [effect](NativeCall& c) {
        const auto* m = effect(c);
        const auto index = static_cast<std::size_t>(std::max(0, arg(c, 0).i()));
        const bool ok = m != nullptr && m->counter_effects() != nullptr && index < m->counter_effects()->size();
        return done(c.vm.object(ok ? m->counter_effects()->Get(static_cast<flatbuffers::uoffset_t>(index)) : 0,
                                "MagicEffect"));
    });
    // IsEffectFlagSet(flag): the flag is a mask of the DATA flags (hostile 1, recover 2, detrimental 4...).
    vm.bind("MagicEffect", "IsEffectFlagSet", [effect](NativeCall& c) {
        const auto* m = effect(c);
        const auto flag = static_cast<std::uint32_t>(arg(c, 0).i());
        return done(Value::boolean(m != nullptr && flag != 0 && (m->flags() & flag) == flag));
    });
}

} // namespace skydot::vm
