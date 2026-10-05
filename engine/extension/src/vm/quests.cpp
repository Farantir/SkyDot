// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus: quests, their aliases and stages, and global variables.
// What the game does and this does not is listed in docs/papyrus.md.
#include "vm/papyrus.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>

using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Variant;

namespace skydot {

namespace {

std::string utf8(const String& s) {
    const auto bytes = s.utf8();
    return {bytes.get_data(), static_cast<std::size_t>(bytes.length())};
}

std::uint32_t u32(const Variant& v) { return static_cast<std::uint32_t>(static_cast<std::int64_t>(v)); }

// Objective state bits.
constexpr std::uint8_t k_displayed = 0x1;
constexpr std::uint8_t k_completed = 0x2;
constexpr std::uint8_t k_failed = 0x4;

} // namespace

const Dictionary& SkydotPapyrus::quest_info(std::uint32_t quest) const {
    auto it = quest_info_.find(quest);
    if (it == quest_info_.end()) {
        it = quest_info_.emplace(quest, world_.is_valid() ? world_->get_quest(quest) : Dictionary()).first;
    }
    return it->second;
}

std::uint32_t SkydotPapyrus::alias_handle(std::uint32_t quest, std::uint32_t alias) {
    const auto key = std::pair{quest, alias};
    if (const auto it = alias_handles_.find(key); it != alias_handles_.end()) {
        return it->second;
    }
    const std::uint32_t handle = next_alias_handle_++;
    alias_handles_[key] = handle;
    handle_alias_[handle] = key;
    return handle;
}

void SkydotPapyrus::attach_quest_scripts(std::uint32_t quest, bool on_init) {
    if (world_.is_null() || quest_scripts_.contains(quest) || !world_->has_quest(quest)) {
        return;
    }
    quest_scripts_.insert(quest);
    const Dictionary& info = quest_info(quest);
    attach_scripts(quest, info["scripts"], quest);
    // Vanilla lists the fragment script among the quest's scripts; attach it
    // if a plugin does not.
    const std::string fragments = utf8(info["fragment_script"]);
    if (!fragments.empty() && vm_->instance(quest, fragments) == nullptr) {
        vm_->attach(quest, fragments);
    }
    std::vector<std::uint32_t> handles;
    const Array aliases = info["aliases"];
    for (std::int64_t i = 0; i < aliases.size(); ++i) {
        const Dictionary alias = aliases[i];
        const Array scripts = alias["scripts"];
        if (!scripts.is_empty()) {
            const auto handle = alias_handle(quest, u32(alias["id"]));
            attach_scripts(handle, scripts, quest);
            handles.push_back(handle);
        }
    }
    if (on_init) {
        vm_->send_event(quest, "OnInit", {});
        for (const auto handle : handles) {
            vm_->send_event(handle, "OnInit", {});
        }
    }
}

// Forced references, unique actors and other quests' aliases fill an alias.
// Created references and conditions ("find matching reference") are not
// supported: those aliases stay empty, required or not.
void SkydotPapyrus::fill_aliases(std::uint32_t quest) {
    const Array aliases = quest_info(quest)["aliases"];
    for (std::int64_t i = 0; i < aliases.size(); ++i) {
        const Dictionary alias = aliases[i];
        std::uint32_t fill = u32(alias["forced"]);
        if (fill == 0 && !static_cast<bool>(alias["location"])) {
            if (const auto npc = u32(alias["unique_actor"]); npc != 0) {
                fill = static_cast<std::uint32_t>(world_->find_actor_of(npc));
            } else if (const auto other = u32(alias["external_quest"]); other != 0) {
                const auto index = static_cast<std::int64_t>(alias["external_alias"]);
                fill = index >= 0 ? static_cast<std::uint32_t>(get_alias_ref(other, index)) : 0;
            }
        }
        alias_fill_[alias_handle(quest, u32(alias["id"]))] = fill;
    }
}

std::int64_t SkydotPapyrus::start_game_enabled_quests() {
    std::int64_t count = 0;
    if (world_.is_null()) {
        return 0;
    }
    const Array quests = world_->list_quests("");
    for (std::int64_t i = 0; i < quests.size(); ++i) {
        const Dictionary q = quests[i];
        if (static_cast<bool>(q["start_game_enabled"]) && start_quest(q["id"])) {
            ++count;
        }
    }
    return count;
}

bool SkydotPapyrus::start_quest(std::int64_t id) {
    const auto quest = static_cast<std::uint32_t>(id);
    const Dictionary& info = quest_info(quest);
    if (info.is_empty()) {
        return false;
    }
    auto& state = quests_[quest];
    if (state.running) {
        return true;
    }
    if (state.started && static_cast<bool>(info["run_once"])) {
        return false;
    }
    // A restart begins again from no stages.
    state = QuestState{};
    state.running = true;
    state.started = true;
    fill_aliases(quest);
    attach_quest_scripts(quest, false);
    // OnInit on every start; the game also sends it when the quest is first
    // initialised, which attach_quest_scripts does for quests named before
    // they start.
    vm_->send_event(quest, "OnInit", {});
    for (const auto& [handle, key] : handle_alias_) {
        if (key.first == quest) {
            vm_->send_event(handle, "OnInit", {});
        }
    }
    emit_signal("quest_started", id);
    const Array stages = info["stages"];
    for (std::int64_t i = 0; i < stages.size(); ++i) {
        const Dictionary stage = stages[i];
        if (static_cast<bool>(stage["start_up"])) {
            set_stage(id, stage["index"]);
            break;
        }
    }
    return true;
}

void SkydotPapyrus::stop_quest(std::int64_t id) {
    const auto quest = static_cast<std::uint32_t>(id);
    const auto it = quests_.find(quest);
    if (it == quests_.end() || !it->second.running) {
        return;
    }
    it->second.running = false;
    for (auto& [handle, fill] : alias_fill_) {
        if (handle_alias_[handle].first == quest) {
            fill = 0;
        }
    }
    emit_signal("quest_stopped", id);
}

bool SkydotPapyrus::set_stage(std::int64_t id, std::int64_t index) {
    const auto quest = static_cast<std::uint32_t>(id);
    const Dictionary& info = quest_info(quest);
    if (info.is_empty()) {
        return false;
    }
    Dictionary stage;
    const Array stages = info["stages"];
    for (std::int64_t i = 0; i < stages.size(); ++i) {
        const Dictionary s = stages[i];
        if (static_cast<std::int64_t>(s["index"]) == index) {
            stage = s;
            break;
        }
    }
    if (stage.is_empty()) {
        return false;
    }
    if (!quests_[quest].running && !start_quest(id)) {
        return false;
    }
    auto& state = quests_[quest];
    const auto value = static_cast<std::int32_t>(index);
    if (state.done.contains(value) && !static_cast<bool>(info["allow_repeated_stages"])) {
        return false;
    }
    state.done.insert(value);
    state.stage = value;

    // Conditions choose among a stage's log entries; they are not evaluated,
    // so the first entry and its fragment are taken.
    const Array log = stage["log"];
    const Dictionary entry = log.is_empty() ? Dictionary() : Dictionary(log[0]);
    const Array fragments = info["fragments"];
    String function;
    for (std::int64_t i = 0; i < fragments.size(); ++i) {
        const Dictionary f = fragments[i];
        if (static_cast<std::int64_t>(f["stage"]) == index) {
            function = f["function"];
            break; // sorted by log entry, so the lowest comes first
        }
    }
    if (!function.is_empty()) {
        auto* instance = vm_->instance(quest, utf8(info["fragment_script"]));
        if (instance == nullptr || vm_->call_method(instance, utf8(function), {}) == 0) {
            vm_->error(utf8(info["editor_id"]) + ": no fragment " + utf8(function) + " for stage " +
                       std::to_string(index));
        }
    }
    emit_signal("quest_stage", id, index, entry.get("text", String()));
    if (static_cast<bool>(entry.get("completes_quest", false))) {
        state.completed = true;
    }
    if (static_cast<bool>(entry.get("fails_quest", false))) {
        state.failed = true;
    }
    if (static_cast<bool>(stage["shut_down"])) {
        stop_quest(id);
    }
    return true;
}

void SkydotPapyrus::set_objective(std::uint32_t quest, std::int32_t index, std::uint8_t bit, bool on) {
    auto& bits = quests_[quest].objectives[index];
    const std::uint8_t was = bits;
    bits = static_cast<std::uint8_t>(on ? (bits | bit) : (bits & ~bit));
    if (bits == was) {
        return;
    }
    String text;
    const Array objectives = quest_info(quest)["objectives"];
    for (std::int64_t i = 0; i < objectives.size(); ++i) {
        const Dictionary o = objectives[i];
        if (static_cast<std::int64_t>(o["index"]) == index) {
            text = o["text"];
            break;
        }
    }
    const char* what = bit == k_displayed ? "displayed" : bit == k_completed ? "completed" : "failed";
    emit_signal("objective_changed", static_cast<std::int64_t>(quest), static_cast<std::int64_t>(index),
                String(on ? what : (String("not ") + what)), text);
}

std::size_t SkydotPapyrus::dispatch(std::uint32_t ref, std::string_view event,
                                    const std::vector<vm::Value>& args) {
    std::size_t count = vm_->send_event(ref, event, args);
    for (const auto& [handle, fill] : alias_fill_) {
        if (fill == ref && fill != 0) {
            count += vm_->send_event(handle, event, args);
        }
    }
    return count;
}

Dictionary SkydotPapyrus::get_quest_state(std::int64_t id) const {
    Dictionary out;
    const auto quest = static_cast<std::uint32_t>(id);
    const auto it = quests_.find(quest);
    if (it == quests_.end()) {
        return out;
    }
    const auto& s = it->second;
    out["running"] = s.running;
    out["completed"] = s.completed;
    out["failed"] = s.failed;
    out["stage"] = s.stage;
    Array done;
    for (const auto stage : s.done) {
        done.push_back(stage);
    }
    out["stages_done"] = done;
    Dictionary objectives;
    for (const auto& [index, bits] : s.objectives) {
        Dictionary o;
        o["displayed"] = (bits & k_displayed) != 0;
        o["completed"] = (bits & k_completed) != 0;
        o["failed"] = (bits & k_failed) != 0;
        objectives[index] = o;
    }
    out["objectives"] = objectives;
    Dictionary aliases;
    for (const auto& [key, handle] : alias_handles_) {
        if (key.first == quest) {
            const auto fill = alias_fill_.find(handle);
            aliases[static_cast<std::int64_t>(key.second)] =
                static_cast<std::int64_t>(fill != alias_fill_.end() ? fill->second : 0);
        }
    }
    out["aliases"] = aliases;
    return out;
}

godot::PackedInt64Array SkydotPapyrus::get_running_quests() const {
    godot::PackedInt64Array out;
    for (const auto& [quest, state] : quests_) {
        if (state.running) {
            out.push_back(quest);
        }
    }
    return out;
}

std::int64_t SkydotPapyrus::get_alias_ref(std::int64_t quest, std::int64_t alias) const {
    const auto it = alias_handles_.find({static_cast<std::uint32_t>(quest), static_cast<std::uint32_t>(alias)});
    if (it == alias_handles_.end()) {
        return 0;
    }
    const auto fill = alias_fill_.find(it->second);
    return fill != alias_fill_.end() ? fill->second : 0;
}

double SkydotPapyrus::get_global_value(std::int64_t global) const {
    if (const auto it = globals_.find(static_cast<std::uint32_t>(global)); it != globals_.end()) {
        return it->second;
    }
    return world_.is_valid() ? static_cast<double>(world_->get_global(global).get("value", 0.0)) : 0.0;
}

void SkydotPapyrus::set_global_value(std::int64_t global, double value) {
    globals_[static_cast<std::uint32_t>(global)] = value;
}

void SkydotPapyrus::bind_quest_natives() {
    using vm::NativeCall;
    using vm::NativeResult;
    using vm::Value;
    auto& v = *vm_;
    const auto arg = [](NativeCall& c, std::size_t i) -> const Value& {
        static const Value none;
        return i < c.args.size() ? c.args[i] : none;
    };
    const auto result = [](Value value) { return NativeResult{.value = std::move(value), .wait = -1.0}; };
    const auto flag = [arg](NativeCall& c, std::size_t i, bool fallback) {
        return c.args.size() > i ? vm::Vm::truthy(arg(c, i)) : fallback;
    };
    const auto state_of = [this](NativeCall& c) -> QuestState& { return quests_[c.self.form()]; };

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
        const bool started = quests_[c.self.form()].started;
        quests_[c.self.form()] = QuestState{};
        quests_[c.self.form()].started = started;
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
            for (const auto& [index, bits] : std::map(quests_[c.self.form()].objectives)) {
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
        const auto it = handle_alias_.find(c.self.form());
        return it != handle_alias_.end() ? result(c.vm.object(it->second.first, "Quest")) : NativeResult{};
    });
    v.bind("Alias", "GetID", [this, result](NativeCall& c) {
        const auto it = handle_alias_.find(c.self.form());
        return result(Value::integer(it != handle_alias_.end() ? static_cast<std::int32_t>(it->second.second) : -1));
    });
    v.bind("Alias", "GetName", [this, result](NativeCall& c) {
        const auto it = handle_alias_.find(c.self.form());
        if (it != handle_alias_.end()) {
            const Array aliases = quest_info(it->second.first).get("aliases", Array());
            for (std::int64_t i = 0; i < aliases.size(); ++i) {
                const Dictionary a = aliases[i];
                if (u32(a["id"]) == it->second.second) {
                    return result(Value::string(utf8(a["name"])));
                }
            }
        }
        return result(Value::string(""));
    });
    const auto fill_of = [this](NativeCall& c) -> std::uint32_t {
        const auto it = alias_fill_.find(c.self.form());
        return it != alias_fill_.end() ? it->second : 0;
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
            alias_fill_[c.self.form()] = arg(c, 0).kind() == vm::Kind::object ? arg(c, 0).form() : 0;
            return NativeResult{};
        });
    }
    v.bind("ReferenceAlias", "ForceRefIfEmpty", [this, arg, fill_of, result](NativeCall& c) {
        if (fill_of(c) != 0 || arg(c, 0).kind() != vm::Kind::object) {
            return result(Value::boolean(false));
        }
        alias_fill_[c.self.form()] = arg(c, 0).form();
        return result(Value::boolean(true));
    });
    for (const char* cls : {"ReferenceAlias", "LocationAlias"}) {
        v.bind(cls, "Clear", [this](NativeCall& c) {
            alias_fill_[c.self.form()] = 0;
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

    // ---- Game.GetForm: any form by id, seen as Form.
    v.bind("Game", "GetForm", [this, arg, result](NativeCall& c) {
        const auto form = static_cast<std::uint32_t>(arg(c, 0).i());
        if (form == 0) {
            return NativeResult{};
        }
        attach_quest_scripts(form, true);
        return result(c.vm.object(form, "Form"));
    });
}

} // namespace skydot
