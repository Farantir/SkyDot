// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/natives_actor.hpp"
#include "vm/natives_args.hpp"
#include "vm/natives_magic.hpp"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace skydot::vm {

namespace {

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
    const std::string name = c.args.empty() ? std::string{} : Vm::to_string(natives::arg(c, 0));
    const auto index = av::index_of(name);
    return index ? index : report("unknown actor value \"" + name + "\"");
}

} // namespace

void bind_actor_natives(Vm& vm, ActorStates& states, std::function<void(std::uint32_t, std::uint32_t, double)> on_mod) {
    using namespace natives;
    // One set for the natives, as the report is about the name or the object.
    auto reported = std::make_shared<std::set<std::string>>();
    const auto reader = [&states, reported](const char* native, double (ActorStates::*read)(std::uint32_t, std::uint32_t) const) {
        return [&states, reported, native, read](NativeCall& c) {
            const auto av = target(c, states, native, *reported);
            return result(Value::floating(av ? static_cast<float>((states.*read)(c.self.form(), *av)) : 0.0F));
        };
    };
    vm.bind("Actor", "GetActorValue", reader("GetActorValue", &ActorStates::get));
    vm.bind("Actor", "GetBaseActorValue", reader("GetBaseActorValue", &ActorStates::get_base));
    vm.bind("Actor", "GetActorValueMax", reader("GetActorValueMax", &ActorStates::max));
    vm.bind("Actor", "GetActorValuePercentage", reader("GetActorValuePercentage", &ActorStates::percentage));

    using Write = void (ActorStates::*)(std::uint32_t, std::uint32_t, double);
    const auto writer = [&states, reported](const char* native, Write write) {
        return [&states, reported, native, write](NativeCall& c) {
            if (const auto av = target(c, states, native, *reported)) {
                (states.*write)(c.self.form(), *av, float_or(c, 1, 0.0));
            }
            return NativeResult{};
        };
    };
    vm.bind("Actor", "SetActorValue", writer("SetActorValue", &ActorStates::set_base));
    vm.bind("Actor", "ModActorValue", [&states, reported, on_mod = std::move(on_mod)](NativeCall& c) {
        if (const auto av = target(c, states, "ModActorValue", *reported)) {
            const double amount = float_or(c, 1, 0.0);
            states.mod(c.self.form(), *av, amount);
            if (on_mod) {
                on_mod(c.self.form(), *av, amount);
            }
        }
        return NativeResult{};
    });
    vm.bind("Actor", "ForceActorValue", writer("ForceActorValue", &ActorStates::force));
    vm.bind("Actor", "DamageActorValue", writer("DamageActorValue", &ActorStates::damage));
    vm.bind("Actor", "RestoreActorValue", writer("RestoreActorValue", &ActorStates::restore));

    // The actor that dealt the killing blow; None while alive or unknown.
    vm.bind("Actor", "GetKiller", [&states](NativeCall& c) {
        const std::uint32_t killer = states.killer_of(c.self.form());
        if (killer == 0) {
            return NativeResult{};
        }
        return result(c.vm.object(killer, "Actor"));
    });

    // Flight (step 7b-6): the settings a script gives a flying actor, kept here and saved; the AI
    // reads them. SetAllowFlyingEx(allowed, crash, search) only differs in what it also lets the
    // game do, which is nothing here.
    const auto allow = [&states](NativeCall& c) {
        states.set_allowed_to_fly(c.self.form(), bool_or(c, 0, true));
        return NativeResult{};
    };
    vm.bind("Actor", "SetAllowFlying", allow);
    vm.bind("Actor", "SetAllowFlyingEx", allow);
    vm.bind("Actor", "IsAllowedToFly", [&states](NativeCall& c) {
        return flag(states.allowed_to_fly(c.self.form()));
    });
    vm.bind("Actor", "SetForcedLandingMarker", [&states](NativeCall& c) {
        states.set_landing_marker(c.self.form(), object_form(c, 0));
        return NativeResult{};
    });
    vm.bind("Actor", "IsDead", [&states, reported](NativeCall& c) {
        if (!states.is_actor(c.self.form())) {
            if (reported->insert("the object is no actor").second) {
                c.vm.error("Actor.IsDead: the object is no actor");
            }
            return flag(false);
        }
        return flag(states.is_dead(c.self.form()));
    });
}

void bind_actor_magic_natives(Vm& vm, MagicApi& api) {
    using namespace natives;
    // AddSpell(akSpell, abVerbose = true): teaches it; a message "<Spell> added" if verbose.
    vm.bind("Actor", "AddSpell", [&api](NativeCall& c) {
        const bool verbose = bool_or(c, 1, true);
        return flag(api.add_spell(c.self.form(), arg(c, 0).form(), verbose));
    });
    vm.bind("Actor", "RemoveSpell", [&api](NativeCall& c) {
        return flag(api.remove_spell(c.self.form(), arg(c, 0).form()));
    });
    vm.bind("Actor", "HasSpell", [&api](NativeCall& c) {
        return flag(api.has_spell(c.self.form(), arg(c, 0).form()));
    });
    vm.bind("Actor", "DispelSpell", [&api](NativeCall& c) {
        return flag(api.dispel_spell(c.self.form(), arg(c, 0).form()));
    });
    vm.bind("Actor", "DispelAllSpells", [&api](NativeCall& c) {
        api.dispel_all_spells(c.self.form());
        return NativeResult{};
    });
    vm.bind("Actor", "HasMagicEffect", [&api](NativeCall& c) {
        return flag(api.has_magic_effect(c.self.form(), arg(c, 0).form()));
    });
    vm.bind("Actor", "HasMagicEffectWithKeyword", [&api](NativeCall& c) {
        return flag(api.has_magic_effect_keyword(c.self.form(), arg(c, 0).form()));
    });
    // DoCombatSpellApply(akSpell, akTarget): the spell hits akTarget as if this actor had cast it.
    vm.bind("Actor", "DoCombatSpellApply", [&api](NativeCall& c) {
        api.cast(arg(c, 0).form(), c.self.form(), 0, arg(c, 1).form());
        return NativeResult{};
    });
    // EquipSpell(akSpell, aiSource): 0 left hand, 1 right hand, 2 instant (the power).
    vm.bind("Actor", "EquipSpell", [&api](NativeCall& c) {
        api.equip_spell(c.self.form(), arg(c, 0).form(), arg(c, 1).i());
        return NativeResult{};
    });
    vm.bind("Actor", "UnequipSpell", [&api](NativeCall& c) {
        api.unequip_spell(c.self.form(), arg(c, 1).i());
        return NativeResult{};
    });
    vm.bind("Actor", "GetEquippedSpell", [&api](NativeCall& c) {
        return result(c.vm.object(api.equipped_spell(c.self.form(), arg(c, 0).i()), "Spell"));
    });
    // ---- shouts
    vm.bind("Actor", "AddShout", [&api](NativeCall& c) {
        return flag(api.add_shout(c.self.form(), arg(c, 0).form()));
    });
    vm.bind("Actor", "RemoveShout", [&api](NativeCall& c) {
        return flag(api.remove_shout(c.self.form(), arg(c, 0).form()));
    });
    vm.bind("Actor", "EquipShout", [&api](NativeCall& c) {
        api.equip_shout(c.self.form(), arg(c, 0).form());
        return NativeResult{};
    });
    vm.bind("Actor", "UnequipShout", [&api](NativeCall& c) {
        api.unequip_shout(c.self.form(), arg(c, 0).form());
        return NativeResult{};
    });
    vm.bind("Actor", "GetEquippedShout", [&api](NativeCall& c) {
        return result(c.vm.object(api.equipped_shout(c.self.form()), "Shout"));
    });
    // SKSE.
    vm.bind("Actor", "SetVoiceRecoveryTime", [&api](NativeCall& c) {
        api.set_voice_recovery(c.self.form(), static_cast<double>(arg(c, 0).f()));
        return NativeResult{};
    });
    vm.bind("Actor", "GetVoiceRecoveryTime", [&api](NativeCall& c) {
        return result(Value::floating(static_cast<float>(api.voice_recovery(c.self.form()))));
    });
    // Game.TeachWord(akWord), UnlockWord(akWord), IsWordUnlocked(akWord): the player's.
    vm.bind("Game", "TeachWord", [&api](NativeCall& c) {
        api.teach_word(arg(c, 0).form());
        return NativeResult{};
    });
    vm.bind("Game", "UnlockWord", [&api](NativeCall& c) {
        api.unlock_word(arg(c, 0).form());
        return NativeResult{};
    });
    vm.bind("Game", "IsWordUnlocked", [&api](NativeCall& c) {
        return flag(api.is_word_unlocked(arg(c, 0).form()));
    });
}

} // namespace skydot::vm
