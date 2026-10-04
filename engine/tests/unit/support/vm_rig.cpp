// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm_rig.hpp"

#include <stdexcept>

namespace skydot::testing {

using vm::NativeCall;
using vm::NativeResult;
using vm::Value;

Rig::Rig() {
    vm.set_loader([this](const std::string& key) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = scripts.find(key);
        if (it == scripts.end()) {
            return std::nullopt;
        }
        return it->second;
    });
    vm.set_log([this](const std::string& message) { log.push_back(message); });

    add({.name = "Util",
         .functions = {native_function("Note", {{"text", "String"}}),
                       native_function("NoteInt", {{"value", "Int"}}),
                       native_function("Triple", {{"value", "Int"}}, "Int"),
                       native_function("Echo", {{"s", "String"}, {"i", "Int"}, {"f", "Float"}, {"b", "Bool"}}),
                       native_function("Wait", {{"seconds", "Float"}}),
                       native_function("WaitFor", {{"key", "String"}, {"timeout", "Float"}})}});
    vm.bind("Util", "Note", [this](NativeCall& c) {
        notes.push_back(c.args.at(0).s);
        return NativeResult{};
    });
    vm.bind("Util", "NoteInt", [this](NativeCall& c) {
        notes.push_back(std::to_string(c.args.at(0).i));
        return NativeResult{};
    });
    vm.bind("Util", "Triple", [](NativeCall& c) {
        return NativeResult{.value = Value::integer(c.args.at(0).i * 3)};
    });
    vm.bind("Util", "Echo", [this](NativeCall& c) {
        echoes.push_back(c.args);
        return NativeResult{};
    });
    vm.bind("Util", "Wait", [](NativeCall& c) {
        return NativeResult{.value = {}, .wait = static_cast<double>(c.args.at(0).f)};
    });
    vm.bind("Util", "WaitFor", [](NativeCall& c) {
        return NativeResult{.value = {}, .wait = static_cast<double>(c.args.at(1).f), .wait_for = c.args.at(0).s};
    });

    add({.name = "ObjectReference",
         .functions = {native_function("GetFormID", {}, "Int", false)}});
    vm.bind("ObjectReference", "GetFormID", [](NativeCall& c) {
        return NativeResult{.value = Value::integer(static_cast<std::int32_t>(c.self.form))};
    });
}

void Rig::add(const ScriptSpec& spec) {
    scripts[vm::to_lower(spec.name)] = build_script(spec);
}

namespace {

Value finish(vm::Vm& vm, std::uint64_t thread) {
    if (thread == 0) {
        throw std::runtime_error("the call is unknown or could not start");
    }
    vm.update(0.0);
    auto result = vm.take_result(thread);
    if (!result) {
        throw std::runtime_error("the call has not finished");
    }
    return std::move(*result);
}

} // namespace

Value Rig::call(std::string_view cls, std::string_view fn, std::vector<Value> args) {
    return finish(vm, vm.call_global(cls, fn, std::move(args)));
}

Value Rig::call(vm::Instance* instance, std::string_view fn, std::vector<Value> args) {
    return finish(vm, vm.call_method(instance, fn, std::move(args)));
}

ScriptSpec script(std::string name, std::vector<FunctionSpec> functions, std::vector<VariableSpec> variables) {
    return {.name = std::move(name),
            .parent = {},
            .auto_state = {},
            .format_version = 1,
            .variables = std::move(variables),
            .properties = {},
            .functions = std::move(functions)};
}

} // namespace skydot::testing
