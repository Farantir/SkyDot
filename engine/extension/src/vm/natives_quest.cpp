// SPDX-License-Identifier: GPL-3.0-or-later
//
// Quest, alias and global variable natives.
#include "vm/natives_common.hpp"
#include "vm/papyrus.hpp"

using skydot::natives::k_completed;
using skydot::natives::k_displayed;
using skydot::natives::k_failed;
using skydot::natives::utf8;
using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Variant;

namespace skydot {

void SkydotPapyrus::bind_quest_natives() {
    using vm::NativeCall;
    using vm::NativeResult;
    using vm::Value;
    using namespace natives;
    [[maybe_unused]] auto& v = *vm_;
    [[maybe_unused]] const auto arg = natives::arg;
    [[maybe_unused]] const auto result = natives::result;
    [[maybe_unused]] const auto number = natives::number;
    const auto flag = [arg](NativeCall& c, std::size_t i, bool fallback) {
        return c.args.size() > i ? vm::Vm::truthy(arg(c, i)) : fallback;
    };
    const auto state_of = [this](NativeCall& c) -> vm::QuestState& { return quests_.state(c.self.form()); };

    // ---- TopicInfo: the INFO a fragment script is attached to
    v.bind("TopicInfo", "GetOwningQuest", [this](NativeCall& c) {
        const auto it = info_quests_.find(c.self.form());
        return NativeResult{.value = it != info_quests_.end() ? c.vm.object(it->second, "Quest") : Value{},
                            .wait = -1.0};
    });

    // ---- Quest
    v.bind("Quest", "Start", [this, result](NativeCall& c) {
        return result(Value::boolean(start_quest(c.self.form())));
    });
    v.bind("Quest", "Stop", [this](NativeCall& c) {
        stop_quest(c.self.form());
        return NativeResult{};
    });
    v.bind("Quest", "Reset", [this](NativeCall& c) {
        stop_quest(c.self.form());
        auto& state = quests_.state(c.self.form());
        const bool started = state.started;
        state = vm::QuestState{};
        state.started = started;
        return NativeResult{};
    });
    v.bind("Quest", "IsRunning", [state_of, result](NativeCall& c) {
        return result(Value::boolean(state_of(c).running));
    });
    v.bind("Quest", "IsStopped", [state_of, result](NativeCall& c) {
        return result(Value::boolean(!state_of(c).running));
    });
    // Starting and stopping are immediate here.
    v.bind("Quest", "IsStarting", [result](NativeCall&) { return result(Value::boolean(false)); });
    v.bind("Quest", "IsStopping", [result](NativeCall&) { return result(Value::boolean(false)); });
    v.bind("Quest", "IsCompleted", [state_of, result](NativeCall& c) {
        return result(Value::boolean(state_of(c).completed));
    });
    v.bind("Quest", "CompleteQuest", [state_of](NativeCall& c) {
        state_of(c).completed = true;
        return NativeResult{};
    });
    for (const char* name : {"GetStage", "GetCurrentStageID"}) {
        v.bind("Quest", name, [state_of, result](NativeCall& c) {
            return result(Value::integer(state_of(c).stage));
        });
    }
    for (const char* name : {"SetStage", "SetCurrentStageID"}) {
        v.bind("Quest", name, [this, arg, result](NativeCall& c) {
            return result(Value::boolean(set_stage(c.self.form(), arg(c, 0).i())));
        });
    }
    for (const char* name : {"GetStageDone", "IsStageDone"}) {
        v.bind("Quest", name, [state_of, arg, result](NativeCall& c) {
            return result(Value::boolean(state_of(c).done.contains(arg(c, 0).i())));
        });
    }
    v.bind("Quest", "IsActive", [state_of, result](NativeCall& c) {
        return result(Value::boolean(state_of(c).active));
    });
    v.bind("Quest", "SetActive", [state_of, flag](NativeCall& c) {
        state_of(c).active = flag(c, 0, true);
        return NativeResult{};
    });
    v.bind("Quest", "GetAlias", [this, arg, result](NativeCall& c) {
        const auto id = arg(c, 0).i();
        const Array aliases = quest_info(c.self.form()).get("aliases", Array());
        for (std::int64_t i = 0; i < aliases.size(); ++i) {
            const Dictionary a = aliases[i];
            if (static_cast<std::int64_t>(a["id"]) == id) {
                return result(c.vm.object(alias_handle(c.self.form(), static_cast<std::uint32_t>(id)),
                                          static_cast<bool>(a["location"]) ? "LocationAlias" : "ReferenceAlias"));
            }
        }
        return NativeResult{};
    });
    const auto objective = [this, arg, flag](std::uint8_t bit) {
        return [this, arg, flag, bit](NativeCall& c) {
            set_objective(c.self.form(), arg(c, 0).i(), bit, flag(c, 1, true));
            return NativeResult{};
        };
    };
    v.bind("Quest", "SetObjectiveDisplayed", objective(k_displayed));
    v.bind("Quest", "SetObjectiveCompleted", objective(k_completed));
    v.bind("Quest", "SetObjectiveFailed", objective(k_failed));
    const auto is_objective = [state_of, arg, result](std::uint8_t bit) {
        return [state_of, arg, result, bit](NativeCall& c) {
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
        v.bind("Quest", name, [this, bit](NativeCall& c) {
            for (const auto& [index, bits] : std::map(quests_.state(c.self.form()).objectives)) {
                if ((bits & k_displayed) != 0) {
                    set_objective(c.self.form(), index, bit, true);
                }
            }
            return NativeResult{};
        });
    }
    // Text replacement with globals is not drawn; the call succeeds.
    v.bind("Quest", "UpdateCurrentInstanceGlobal", [result](NativeCall&) {
        return result(Value::boolean(true));
    });
    v.bind("Quest", "PrepareForReinitializing", [](NativeCall&) { return NativeResult{}; });

    // ---- Alias, ReferenceAlias, LocationAlias. self.form is the handle.
    v.bind("Alias", "GetOwningQuest", [this, result](NativeCall& c) {
        const auto* owner = quests_.owner(c.self.form());
        return owner != nullptr ? result(c.vm.object(owner->first, "Quest")) : NativeResult{};
    });
    v.bind("Alias", "GetID", [this, result](NativeCall& c) {
        const auto* owner = quests_.owner(c.self.form());
        return result(Value::integer(owner != nullptr ? static_cast<std::int32_t>(owner->second) : -1));
    });
    v.bind("Alias", "GetName", [this, result](NativeCall& c) {
        if (const auto* owner = quests_.owner(c.self.form())) {
            const Array aliases = quest_info(owner->first).get("aliases", Array());
            for (std::int64_t i = 0; i < aliases.size(); ++i) {
                const Dictionary a = aliases[i];
                if (form_of(a["id"]) == owner->second) {
                    return result(Value::string(utf8(a["name"])));
                }
            }
        }
        return result(Value::string(""));
    });
    const auto fill_of = [this](NativeCall& c) -> std::uint32_t {
        return quests_.fill(c.self.form());
    };
    for (const auto& [name, cls] : {std::pair{"GetReference", "ObjectReference"},
                                    std::pair{"GetRef", "ObjectReference"},
                                    std::pair{"GetActorReference", "Actor"},
                                    std::pair{"GetActorRef", "Actor"}}) {
        v.bind("ReferenceAlias", name, [fill_of, result, cls](NativeCall& c) {
            const auto ref = fill_of(c);
            return ref != 0 ? result(c.vm.object(ref, cls)) : NativeResult{};
        });
    }
    v.bind("LocationAlias", "GetLocation", [fill_of, result](NativeCall& c) {
        const auto loc = fill_of(c);
        return loc != 0 ? result(c.vm.object(loc, "Location")) : NativeResult{};
    });
    for (const auto& [cls, name] : {std::pair{"ReferenceAlias", "ForceRefTo"},
                                    std::pair{"LocationAlias", "ForceLocationTo"}}) {
        v.bind(cls, name, [this, arg](NativeCall& c) {
            quests_.set_fill(c.self.form(), arg(c, 0).kind() == vm::Kind::object ? arg(c, 0).form() : 0);
            return NativeResult{};
        });
    }
    v.bind("ReferenceAlias", "ForceRefIfEmpty", [this, arg, fill_of, result](NativeCall& c) {
        if (fill_of(c) != 0 || arg(c, 0).kind() != vm::Kind::object) {
            return result(Value::boolean(false));
        }
        quests_.set_fill(c.self.form(), arg(c, 0).form());
        return result(Value::boolean(true));
    });
    for (const char* cls : {"ReferenceAlias", "LocationAlias"}) {
        v.bind(cls, "Clear", [this](NativeCall& c) {
            quests_.set_fill(c.self.form(), 0);
            return NativeResult{};
        });
    }

    // ---- GlobalVariable
    v.bind("GlobalVariable", "GetValue", [this, result](NativeCall& c) {
        return result(Value::floating(static_cast<float>(get_global_value(c.self.form()))));
    });
    v.bind("GlobalVariable", "SetValue", [this, arg](NativeCall& c) {
        const auto& x = arg(c, 0);
        set_global_value(c.self.form(), x.kind() == vm::Kind::integer ? static_cast<double>(x.i()) : static_cast<double>(x.f()));
        return NativeResult{};
    });

    v.bind("Game", "GetFormFromFile", [this, arg, result](NativeCall& c) {
        const auto form = static_cast<std::uint32_t>(
            world_->get_form_from_file(arg(c, 0).i(), String::utf8(arg(c, 1).s().c_str())));
        if (form == 0) {
            return NativeResult{};
        }
        attach_quest_scripts(form, true);
        return result(c.vm.object(form, "Form"));
    });

    // Aliases register for updates as forms do; OnUpdate goes to the alias's script.
    const auto updates = [arg](NativeCall& c, bool repeat) {
        if (c.self.instance() != nullptr) {
            const double interval = std::max(0.0, arg(c, 0).kind() == vm::Kind::floating
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

} // namespace skydot
