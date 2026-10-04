// SPDX-License-Identifier: GPL-3.0-or-later
//
// A VM set up as SkydotPapyrus sets one up (vm/papyrus.cpp): a loader that
// finds classes by lowercase name, a log, natives bound by class and
// function. The scripts are built in memory, and two engine types the tests
// share are there from the start:
//
//   Util              Note(String), NoteInt(Int), Triple(Int) -> Int,
//                     Echo(String, Int, Float, Bool), the latent
//                     Wait(Float) and WaitFor(String key, Float timeout)
//   ObjectReference   GetFormID() -> Int, a method of the object it is
//                     called on
//
// Util.Note and NoteInt append to `notes` in the order they run, which is
// how the tests see which thread did what, and when.
#pragma once

#include "script_builder.hpp"

#include "vm/vm.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace skydot::testing {

struct Rig {
    vm::Vm vm;
    /// Script assets by lowercase class name: what the loader serves.
    std::map<std::string, std::vector<std::uint8_t>> scripts;
    /// What the VM reported (`Vm::error`), in order.
    std::vector<std::string> log;
    /// What `Util.Note` and `Util.NoteInt` were given, in order.
    std::vector<std::string> notes;
    /// The arguments of each `Util.Echo`, as the native received them.
    std::vector<std::vector<vm::Value>> echoes;

    Rig();
    // The VM's callbacks point at this.
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    void add(const ScriptSpec& spec);

    /// Start a global function, run the VM once without advancing the clock,
    /// and return its result. Throws if the call is unknown or has not
    /// finished (it waits, or used its budget).
    vm::Value call(std::string_view cls, std::string_view fn, std::vector<vm::Value> args = {});
    /// The same for a method of `instance`.
    vm::Value call(vm::Instance* instance, std::string_view fn, std::vector<vm::Value> args = {});
};

// Values as the tests write them, named for the Papyrus types.
[[nodiscard]] inline vm::Value Int(std::int32_t v) { return vm::Value::integer(v); }
[[nodiscard]] inline vm::Value Float(float v) { return vm::Value::floating(v); }
[[nodiscard]] inline vm::Value Bool(bool v) { return vm::Value::boolean(v); }
[[nodiscard]] inline vm::Value String(std::string v) { return vm::Value::string(std::move(v)); }

/// `Util.Note(text)` as the next instruction of `f`.
inline void note(FunctionSpec& f, const std::string& text) { call_static(f, "Util", "Note", {str(text)}); }

/// A script as a class `name` with the global functions and variables given.
[[nodiscard]] ScriptSpec script(std::string name, std::vector<FunctionSpec> functions,
                                std::vector<VariableSpec> variables = {});

} // namespace skydot::testing
