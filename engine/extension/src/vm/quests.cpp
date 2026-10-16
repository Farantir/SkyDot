// SPDX-License-Identifier: GPL-3.0-or-later
//
// SkydotPapyrus: quests, their aliases and stages, and global variables.
// What the game does and this does not is listed in docs/papyrus.md.
#include "vm/natives_common.hpp"
#include "vm/papyrus.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>

using godot::Array;
using godot::Dictionary;
using godot::String;
using godot::Variant;

namespace skydot {

namespace {

using natives::k_completed;
using natives::k_displayed;
using natives::k_failed;
using natives::utf8;

} // namespace

const Dictionary& SkydotPapyrus::quest_info(std::uint32_t quest) const {
    auto it = quest_info_.find(quest);
    if (it == quest_info_.end()) {
        it = quest_info_.emplace(quest, world_.is_valid() ? world_->get_quest(quest) : Dictionary()).first;
    }
    return it->second;
}

std::uint32_t SkydotPapyrus::alias_handle(std::uint32_t quest, std::uint32_t alias) {
    if (const std::uint32_t handle = quests_.find_handle(quest, alias); handle != 0) {
        return handle;
    }
    const std::uint32_t handle = quests_.next_handle++;
    quests_.add_handle(quest, alias, handle);
    return handle;
}

void SkydotPapyrus::attach_quest_scripts(std::uint32_t quest, bool on_init) {
    if (world_.is_null() || quests_.scripts_attached(quest) || !world_->has_quest(quest)) {
        return;
    }
    quests_.mark_scripts_attached(quest);
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
            const auto handle = alias_handle(quest, form_of(alias["id"]));
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
        std::uint32_t fill = form_of(alias["forced"]);
        if (fill == 0 && !static_cast<bool>(alias["location"])) {
            if (const auto npc = form_of(alias["unique_actor"]); npc != 0) {
                fill = static_cast<std::uint32_t>(world_->find_actor_of(npc));
            } else if (const auto other = form_of(alias["external_quest"]); other != 0) {
                const auto index = static_cast<std::int64_t>(alias["external_alias"]);
                fill = index >= 0 ? static_cast<std::uint32_t>(get_alias_ref(other, index)) : 0;
            }
        }
        quests_.set_fill(alias_handle(quest, form_of(alias["id"])), fill);
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
    auto& state = quests_.state(quest);
    if (state.running) {
        return true;
    }
    if (state.started && static_cast<bool>(info["run_once"])) {
        return false;
    }
    // A restart begins again from no stages.
    state = vm::QuestState{};
    state.running = true;
    state.started = true;
    fill_aliases(quest);
    attach_quest_scripts(quest, false);
    // OnInit on every start; the game also sends it when the quest is first
    // initialised, which attach_quest_scripts does for quests named before
    // they start.
    vm_->send_event(quest, "OnInit", {});
    // The quest's alias objects in the order they were made.
    for (const auto handle : quests_.handles_of(quest)) {
        vm_->send_event(handle, "OnInit", {});
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
    const vm::QuestState* found = quests_.find(quest);
    if (found == nullptr || !found->running) {
        return;
    }
    quests_.state(quest).running = false;
    quests_.clear_fills(quest);
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
    if (!quests_.state(quest).running && !start_quest(id)) {
        return false;
    }
    auto& state = quests_.state(quest);
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
    auto& bits = quests_.state(quest).objectives[index];
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
    for (const auto& [handle, fill] : quests_.fills()) {
        if (fill == ref && fill != 0) {
            count += vm_->send_event(handle, event, args);
        }
    }
    return count;
}

Dictionary SkydotPapyrus::get_quest_state(std::int64_t id) const {
    Dictionary out;
    const auto quest = static_cast<std::uint32_t>(id);
    const vm::QuestState* found = quests_.find(quest);
    if (found == nullptr) {
        return out;
    }
    const auto& s = *found;
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
    for (const auto& [alias, handle] : quests_.aliases_of(quest)) {
        aliases[static_cast<std::int64_t>(alias)] = static_cast<std::int64_t>(quests_.fill(handle));
    }
    out["aliases"] = aliases;
    return out;
}

godot::PackedInt64Array SkydotPapyrus::get_running_quests() const {
    godot::PackedInt64Array out;
    for (const auto& [quest, state] : quests_.quests()) {
        if (state.running) {
            out.push_back(quest);
        }
    }
    return out;
}

std::int64_t SkydotPapyrus::get_alias_ref(std::int64_t quest, std::int64_t alias) const {
    return quests_.fill(quests_.find_handle(static_cast<std::uint32_t>(quest), static_cast<std::uint32_t>(alias)));
}

SkydotPapyrus::ClockGlobal SkydotPapyrus::clock_global(std::uint32_t global) const {
    if (clock_.is_null() || world_.is_null()) {
        return ClockGlobal::none;
    }
    const String id = String(world_->get_global(global).get("editor_id", String())).to_lower();
    if (id == "gamehour") {
        return ClockGlobal::hour;
    }
    if (id == "gamedayspassed") {
        return ClockGlobal::days_passed;
    }
    if (id == "timescale") {
        return ClockGlobal::time_scale;
    }
    return ClockGlobal::none;
}

double SkydotPapyrus::get_global_value(std::int64_t global) const {
    switch (clock_global(static_cast<std::uint32_t>(global))) {
    case ClockGlobal::hour: return clock_->get_hour();
    case ClockGlobal::days_passed: return clock_->get_game_days();
    case ClockGlobal::time_scale: return clock_->get_time_scale();
    case ClockGlobal::none: break;
    }
    if (const double* value = quests_.global(static_cast<std::uint32_t>(global))) {
        return *value;
    }
    return world_.is_valid() ? static_cast<double>(world_->get_global(global).get("value", 0.0)) : 0.0;
}

void SkydotPapyrus::set_global_value(std::int64_t global, double value) {
    switch (clock_global(static_cast<std::uint32_t>(global))) {
    case ClockGlobal::hour: clock_->set_hour(value); return;
    case ClockGlobal::days_passed: clock_->set_game_days(value); return;
    case ClockGlobal::time_scale: clock_->set_time_scale(value); return;
    case ClockGlobal::none: break;
    }
    quests_.set_global(static_cast<std::uint32_t>(global), value);
}

} // namespace skydot
