// SPDX-License-Identifier: GPL-3.0-or-later
//
// Quest, alias and global variable natives.
#include "vm/natives_args.hpp"
#include "vm/natives_families.hpp"

#include <algorithm>
#include <map>

namespace skydot::vm {

void bind_quest_natives(Vm& v, const WorldAccess& world, QuestHost& host) {
    using namespace natives;
    const auto flag = [](NativeCall& c, std::size_t i, bool fallback) {
        return c.args.size() > i ? Vm::truthy(arg(c, i)) : fallback;
    };
    const auto state_of = [&host](NativeCall& c) -> QuestState& { return host.quests().state(c.self.form()); };

    // ---- TopicInfo: the INFO a fragment script is attached to
    v.bind("TopicInfo", "GetOwningQuest", [&host](NativeCall& c) {
        std::uint32_t quest = 0;
        const bool found = host.owning_quest(c.self.form(), quest);
        return NativeResult{.value = found ? c.vm.object(quest, "Quest") : Value{}, .wait = -1.0};
    });

    // ---- Quest
    v.bind("Quest", "Start", [&host](NativeCall& c) {
        return result(Value::boolean(host.start_quest(c.self.form())));
    });
    v.bind("Quest", "Stop", [&host](NativeCall& c) {
        host.stop_quest(c.self.form());
        return NativeResult{};
    });
    v.bind("Quest", "Reset", [&host](NativeCall& c) {
        host.stop_quest(c.self.form());
        auto& state = host.quests().state(c.self.form());
        const bool started = state.started;
        state = QuestState{};
        state.started = started;
        return NativeResult{};
    });
    v.bind("Quest", "IsRunning", [state_of](NativeCall& c) {
        return result(Value::boolean(state_of(c).running));
    });
    v.bind("Quest", "IsStopped", [state_of](NativeCall& c) {
        return result(Value::boolean(!state_of(c).running));
    });
    // Starting and stopping are immediate here.
    v.bind("Quest", "IsStarting", [](NativeCall&) { return result(Value::boolean(false)); });
    v.bind("Quest", "IsStopping", [](NativeCall&) { return result(Value::boolean(false)); });
    v.bind("Quest", "IsCompleted", [state_of](NativeCall& c) {
        return result(Value::boolean(state_of(c).completed));
    });
    v.bind("Quest", "CompleteQuest", [state_of](NativeCall& c) {
        state_of(c).completed = true;
        return NativeResult{};
    });
    for (const char* name : {"GetStage", "GetCurrentStageID"}) {
        v.bind("Quest", name, [state_of](NativeCall& c) {
            return result(Value::integer(state_of(c).stage));
        });
    }
    for (const char* name : {"SetStage", "SetCurrentStageID"}) {
        v.bind("Quest", name, [&host](NativeCall& c) {
            return result(Value::boolean(host.set_stage(c.self.form(), arg(c, 0).i())));
        });
    }
    for (const char* name : {"GetStageDone", "IsStageDone"}) {
        v.bind("Quest", name, [state_of](NativeCall& c) {
            return result(Value::boolean(state_of(c).done.contains(arg(c, 0).i())));
        });
    }
    v.bind("Quest", "IsActive", [state_of](NativeCall& c) {
        return result(Value::boolean(state_of(c).active));
    });
    v.bind("Quest", "SetActive", [state_of, flag](NativeCall& c) {
        state_of(c).active = flag(c, 0, true);
        return NativeResult{};
    });
    v.bind("Quest", "GetAlias", [&host](NativeCall& c) {
        const auto id = arg(c, 0).i();
        AliasInfo alias;
        if (!host.alias_info(c.self.form(), id, alias)) {
            return NativeResult{};
        }
        return result(c.vm.object(host.alias_handle(c.self.form(), static_cast<std::uint32_t>(id)),
                                  alias.location ? "LocationAlias" : "ReferenceAlias"));
    });
    const auto objective = [&host, flag](std::uint8_t bit) {
        return [&host, flag, bit](NativeCall& c) {
            host.set_objective(c.self.form(), arg(c, 0).i(), bit, flag(c, 1, true));
            return NativeResult{};
        };
    };
    v.bind("Quest", "SetObjectiveDisplayed", objective(k_displayed));
    v.bind("Quest", "SetObjectiveCompleted", objective(k_completed));
    v.bind("Quest", "SetObjectiveFailed", objective(k_failed));
    const auto is_objective = [state_of](std::uint8_t bit) {
        return [state_of, bit](NativeCall& c) {
            const auto& objectives = state_of(c).objectives;
            const auto it = objectives.find(arg(c, 0).i());
            return result(Value::boolean(it != objectives.end() && (it->second & bit) != 0));
        };
    };
    v.bind("Quest", "IsObjectiveDisplayed", is_objective(k_displayed));
    v.bind("Quest", "IsObjectiveCompleted", is_objective(k_completed));
    v.bind("Quest", "IsObjectiveFailed", is_objective(k_failed));
    for (const auto& [name, bit] : {std::pair{"CompleteAllObjectives", k_completed},
                                    std::pair{"FailAllObjectives", k_failed}}) {
        v.bind("Quest", name, [&host, bit](NativeCall& c) {
            for (const auto& [index, bits] : std::map(host.quests().state(c.self.form()).objectives)) {
                if ((bits & k_displayed) != 0) {
                    host.set_objective(c.self.form(), index, bit, true);
                }
            }
            return NativeResult{};
        });
    }
    // Text replacement with globals is not drawn; the call succeeds.
    v.bind("Quest", "UpdateCurrentInstanceGlobal", [](NativeCall&) {
        return result(Value::boolean(true));
    });
    v.bind("Quest", "PrepareForReinitializing", [](NativeCall&) { return NativeResult{}; });

    // ---- Alias, ReferenceAlias, LocationAlias. self.form is the handle.
    v.bind("Alias", "GetOwningQuest", [&host](NativeCall& c) {
        const auto* owner = host.quests().owner(c.self.form());
        return owner != nullptr ? result(c.vm.object(owner->first, "Quest")) : NativeResult{};
    });
    v.bind("Alias", "GetID", [&host](NativeCall& c) {
        const auto* owner = host.quests().owner(c.self.form());
        return result(Value::integer(owner != nullptr ? static_cast<std::int32_t>(owner->second) : -1));
    });
    v.bind("Alias", "GetName", [&host](NativeCall& c) {
        if (const auto* owner = host.quests().owner(c.self.form())) {
            AliasInfo alias;
            if (host.alias_info(owner->first, static_cast<std::int64_t>(owner->second), alias)) {
                return result(Value::string(alias.name));
            }
        }
        return result(Value::string(""));
    });
    const auto fill_of = [&host](NativeCall& c) -> std::uint32_t {
        return host.quests().fill(c.self.form());
    };
    for (const auto& [name, cls] : {std::pair{"GetReference", "ObjectReference"},
                                    std::pair{"GetRef", "ObjectReference"},
                                    std::pair{"GetActorReference", "Actor"},
                                    std::pair{"GetActorRef", "Actor"}}) {
        v.bind("ReferenceAlias", name, [fill_of, cls](NativeCall& c) {
            const auto ref = fill_of(c);
            return ref != 0 ? result(c.vm.object(ref, cls)) : NativeResult{};
        });
    }
    v.bind("LocationAlias", "GetLocation", [fill_of](NativeCall& c) {
        const auto loc = fill_of(c);
        return loc != 0 ? result(c.vm.object(loc, "Location")) : NativeResult{};
    });
    for (const auto& [cls, name] : {std::pair{"ReferenceAlias", "ForceRefTo"},
                                    std::pair{"LocationAlias", "ForceLocationTo"}}) {
        v.bind(cls, name, [&host](NativeCall& c) {
            host.quests().set_fill(c.self.form(), arg(c, 0).kind() == Kind::object ? arg(c, 0).form() : 0);
            return NativeResult{};
        });
    }
    v.bind("ReferenceAlias", "ForceRefIfEmpty", [&host, fill_of](NativeCall& c) {
        if (fill_of(c) != 0 || arg(c, 0).kind() != Kind::object) {
            return result(Value::boolean(false));
        }
        host.quests().set_fill(c.self.form(), arg(c, 0).form());
        return result(Value::boolean(true));
    });
    for (const char* cls : {"ReferenceAlias", "LocationAlias"}) {
        v.bind(cls, "Clear", [&host](NativeCall& c) {
            host.quests().set_fill(c.self.form(), 0);
            return NativeResult{};
        });
    }

    // ---- GlobalVariable
    v.bind("GlobalVariable", "GetValue", [&host](NativeCall& c) {
        return result(Value::floating(static_cast<float>(host.global_value(c.self.form()))));
    });
    v.bind("GlobalVariable", "SetValue", [&host](NativeCall& c) {
        const auto& x = arg(c, 0);
        host.set_global_value(c.self.form(),
                              x.kind() == Kind::integer ? static_cast<double>(x.i()) : static_cast<double>(x.f()));
        return NativeResult{};
    });

    v.bind("Game", "GetFormFromFile", [&world, &host](NativeCall& c) {
        const auto form = world.form_from_file(arg(c, 0).i(), arg(c, 1).s());
        if (form == 0) {
            return NativeResult{};
        }
        host.attach_quest_scripts(form, true);
        return result(c.vm.object(form, "Form"));
    });

    // Aliases register for updates as forms do; OnUpdate goes to the alias's script.
    const auto updates = [](NativeCall& c, bool repeat) {
        if (c.self.instance() != nullptr) {
            const double interval = std::max(0.0, arg(c, 0).kind() == Kind::floating
                                                      ? static_cast<double>(arg(c, 0).f())
                                                      : static_cast<double>(arg(c, 0).i()));
            c.vm.register_update(c.self.instance(), interval, repeat ? std::max(interval, 0.001) : 0.0);
        }
        return NativeResult{};
    };
    v.bind("Alias", "RegisterForSingleUpdate", [updates](NativeCall& c) { return updates(c, false); });
    v.bind("Alias", "RegisterForUpdate", [updates](NativeCall& c) { return updates(c, true); });
    v.bind("Alias", "UnregisterForUpdate", [](NativeCall& c) {
        if (c.self.instance() != nullptr) {
            c.vm.unregister_update(c.self.instance());
        }
        return NativeResult{};
    });
}

} // namespace skydot::vm
