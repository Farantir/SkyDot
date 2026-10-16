// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/natives_actor.hpp"

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

} // namespace skydot::vm
