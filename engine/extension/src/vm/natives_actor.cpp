// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/natives_actor.hpp"
#include "vm/natives_magic.hpp"

#include <memory>
#include <optional>
#include <set>
#include <string>

namespace skydot::vm {

namespace {

double number(const Value& x) {
    return x.kind() == Kind::floating ? static_cast<double>(x.f())
                                      : (x.kind() == Kind::integer ? static_cast<double>(x.i()) : 0.0);
}

/// The actor value index `call` names for actor `self`, reporting (once per
/// reason) and returning none when it cannot be used.
std::optional<std::uint32_t> target(NativeCall& c, const ActorStates& states, const char* native,
                                    std::set<std::string>& reported) {
    const auto report = [&](const std::string& what) {
        if (reported.insert(what).second) {
            c.vm.error(std::string("Actor.") + native + ": " + what);
        }
        return std::optional<std::uint32_t>{};
    };
    if (!states.is_actor(c.self.form())) {
        return report("the object is no actor");
    }
    const std::string name = c.args.empty() ? std::string{} : Vm::to_string(c.args[0]);
    const auto index = av::index_of(name);
    return index ? index : report("unknown actor value \"" + name + "\"");
}

} // namespace

void bind_actor_natives(Vm& vm, ActorStates& states) {
    // One set for the natives, as the report is about the name or the object.
    auto reported = std::make_shared<std::set<std::string>>();
    const auto reader = [&states, reported](const char* native, double (ActorStates::*read)(std::uint32_t, std::uint32_t) const) {
        return [&states, reported, native, read](NativeCall& c) {
            const auto av = target(c, states, native, *reported);
            return NativeResult{.value = Value::floating(av ? static_cast<float>((states.*read)(c.self.form(), *av)) : 0.0F),
                                .wait = -1.0};
        };
    };
    vm.bind("Actor", "GetActorValue", reader("GetActorValue", &ActorStates::get));
    vm.bind("Actor", "GetBaseActorValue", reader("GetBaseActorValue", &ActorStates::get_base));
    vm.bind("Actor", "GetActorValuePercentage", reader("GetActorValuePercentage", &ActorStates::percentage));

    using Write = void (ActorStates::*)(std::uint32_t, std::uint32_t, double);
    const auto writer = [&states, reported](const char* native, Write write) {
        return [&states, reported, native, write](NativeCall& c) {
            if (const auto av = target(c, states, native, *reported)) {
                (states.*write)(c.self.form(), *av, c.args.size() > 1 ? number(c.args[1]) : 0.0);
            }
            return NativeResult{};
        };
    };
    vm.bind("Actor", "SetActorValue", writer("SetActorValue", &ActorStates::set_base));
    vm.bind("Actor", "ModActorValue", writer("ModActorValue", &ActorStates::mod));
    vm.bind("Actor", "ForceActorValue", writer("ForceActorValue", &ActorStates::force));
    vm.bind("Actor", "DamageActorValue", writer("DamageActorValue", &ActorStates::damage));
    vm.bind("Actor", "RestoreActorValue", writer("RestoreActorValue", &ActorStates::restore));

    // The actor that dealt the killing blow; None while alive or unknown.
    vm.bind("Actor", "GetKiller", [&states](NativeCall& c) {
        const std::uint32_t killer = states.killer_of(c.self.form());
        if (killer == 0) {
            return NativeResult{};
        }
        return NativeResult{.value = c.vm.object(killer, "Actor"), .wait = -1.0};
    });

    vm.bind("Actor", "IsDead", [&states, reported](NativeCall& c) {
        if (!states.is_actor(c.self.form())) {
            if (reported->insert("the object is no actor").second) {
                c.vm.error("Actor.IsDead: the object is no actor");
            }
            return NativeResult{.value = Value::boolean(false), .wait = -1.0};
        }
        return NativeResult{.value = Value::boolean(states.is_dead(c.self.form())), .wait = -1.0};
    });
}

void bind_actor_magic_natives(Vm& vm, MagicApi& api) {
    const auto arg = [](NativeCall& c, std::size_t i) -> const Value& {
        static const Value none;
        return i < c.args.size() ? c.args[i] : none;
    };
    const auto flag = [](bool v) { return NativeResult{.value = Value::boolean(v), .wait = -1.0}; };
    // AddSpell(akSpell, abVerbose = true): teaches it; a message "<Spell> added" if verbose.
    vm.bind("Actor", "AddSpell", [&api, arg, flag](NativeCall& c) {
        const bool verbose = c.args.size() < 2 || Vm::truthy(c.args[1]);
        return flag(api.add_spell(c.self.form(), arg(c, 0).form(), verbose));
    });
    vm.bind("Actor", "RemoveSpell", [&api, arg, flag](NativeCall& c) {
        return flag(api.remove_spell(c.self.form(), arg(c, 0).form()));
    });
    vm.bind("Actor", "HasSpell", [&api, arg, flag](NativeCall& c) {
        return flag(api.has_spell(c.self.form(), arg(c, 0).form()));
    });
    vm.bind("Actor", "DispelSpell", [&api, arg, flag](NativeCall& c) {
        return flag(api.dispel_spell(c.self.form(), arg(c, 0).form()));
    });
    vm.bind("Actor", "DispelAllSpells", [&api](NativeCall& c) {
        api.dispel_all_spells(c.self.form());
        return NativeResult{};
    });
    vm.bind("Actor", "HasMagicEffect", [&api, arg, flag](NativeCall& c) {
        return flag(api.has_magic_effect(c.self.form(), arg(c, 0).form()));
    });
    vm.bind("Actor", "HasMagicEffectWithKeyword", [&api, arg, flag](NativeCall& c) {
        return flag(api.has_magic_effect_keyword(c.self.form(), arg(c, 0).form()));
    });
    // DoCombatSpellApply(akSpell, akTarget): the spell hits akTarget as if this actor had cast it.
    vm.bind("Actor", "DoCombatSpellApply", [&api, arg](NativeCall& c) {
        api.cast(arg(c, 0).form(), c.self.form(), 0, arg(c, 1).form());
        return NativeResult{};
    });
    // EquipSpell(akSpell, aiSource): 0 left hand, 1 right hand, 2 instant (the power).
    vm.bind("Actor", "EquipSpell", [&api, arg](NativeCall& c) {
        api.equip_spell(c.self.form(), arg(c, 0).form(), arg(c, 1).i());
        return NativeResult{};
    });
    vm.bind("Actor", "UnequipSpell", [&api, arg](NativeCall& c) {
        api.unequip_spell(c.self.form(), arg(c, 1).i());
        return NativeResult{};
    });
    vm.bind("Actor", "GetEquippedSpell", [&api, arg](NativeCall& c) {
        return NativeResult{.value = c.vm.object(api.equipped_spell(c.self.form(), arg(c, 0).i()), "Spell"),
                            .wait = -1.0};
    });
    // ---- shouts
    vm.bind("Actor", "AddShout", [&api, arg, flag](NativeCall& c) {
        return flag(api.add_shout(c.self.form(), arg(c, 0).form()));
    });
    vm.bind("Actor", "RemoveShout", [&api, arg, flag](NativeCall& c) {
        return flag(api.remove_shout(c.self.form(), arg(c, 0).form()));
    });
    vm.bind("Actor", "EquipShout", [&api, arg](NativeCall& c) {
        api.equip_shout(c.self.form(), arg(c, 0).form());
        return NativeResult{};
    });
    vm.bind("Actor", "UnequipShout", [&api, arg](NativeCall& c) {
        api.unequip_shout(c.self.form(), arg(c, 0).form());
        return NativeResult{};
    });
    vm.bind("Actor", "GetEquippedShout", [&api](NativeCall& c) {
        return NativeResult{.value = c.vm.object(api.equipped_shout(c.self.form()), "Shout"), .wait = -1.0};
    });
    // SKSE.
    vm.bind("Actor", "SetVoiceRecoveryTime", [&api, arg](NativeCall& c) {
        api.set_voice_recovery(c.self.form(), static_cast<double>(arg(c, 0).f()));
        return NativeResult{};
    });
    vm.bind("Actor", "GetVoiceRecoveryTime", [&api](NativeCall& c) {
        return NativeResult{.value = Value::floating(static_cast<float>(api.voice_recovery(c.self.form()))), .wait = -1.0};
    });
    // Game.TeachWord(akWord), UnlockWord(akWord), IsWordUnlocked(akWord): the player's.
    vm.bind("Game", "TeachWord", [&api, arg](NativeCall& c) {
        api.teach_word(arg(c, 0).form());
        return NativeResult{};
    });
    vm.bind("Game", "UnlockWord", [&api, arg](NativeCall& c) {
        api.unlock_word(arg(c, 0).form());
        return NativeResult{};
    });
    vm.bind("Game", "IsWordUnlocked", [&api, arg, flag](NativeCall& c) {
        return flag(api.is_word_unlocked(arg(c, 0).form()));
    });
}

} // namespace skydot::vm
