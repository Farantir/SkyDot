// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small assembler for script assets (formats/schema/script.fbs), written
// with the generated builder API so a test states its program instruction by
// instruction. Strings go into the table as they are used. Jumps name labels
// and are turned into the relative offsets the schema wants.
//
// Arguments are written as in the converter's PEX fixtures (none, ident, str,
// integer, floating, boolean), so the two halves read alike.
#pragma once

#include "vm/script_class.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skydot::testing {

/// An argument: the schema's Value, with strings by text, not by index.
struct Arg {
    std::uint8_t type = 0; ///< 0 none, 1 identifier, 2 string, 3 int, 4 float, 5 bool.
    std::string text;
    std::uint32_t bits = 0;
    /// For identifiers and strings: `bits` is the string table index as it
    /// stands, to write one that is out of range.
    bool raw = false;
};

[[nodiscard]] inline Arg none() { return {}; }
/// A register, script variable, `self`, `::State`, or the name a call is to.
[[nodiscard]] inline Arg ident(std::string name) { return {.type = 1, .text = std::move(name)}; }
[[nodiscard]] inline Arg str(std::string text) { return {.type = 2, .text = std::move(text)}; }
[[nodiscard]] inline Arg integer(std::int32_t v) {
    return {.type = 3, .text = {}, .bits = static_cast<std::uint32_t>(v)};
}
[[nodiscard]] Arg floating(float v);
[[nodiscard]] inline Arg boolean(bool v) { return {.type = 5, .text = {}, .bits = v ? 1U : 0U}; }
/// An identifier whose table index is `index`, whatever the table holds.
[[nodiscard]] inline Arg raw_ident(std::uint32_t index) {
    return {.type = 1, .text = {}, .bits = index, .raw = true};
}

struct Param {
    std::string name;
    std::string type; ///< As Papyrus spells it: Int, Float, Bool, String, Foo, Int[]...
};

struct Instruction {
    vm::Op op = vm::Op::nop;
    /// The fixed arguments, then a call's variable ones; no count.
    std::vector<Arg> args;
    /// For a jump: the label it goes to, whose offset is appended to `args`.
    std::string target;
};

struct FunctionSpec {
    std::string name;
    /// The state it is in; empty for the default state.
    std::string state = {};
    std::string return_type = "None";
    bool global = false;
    bool native = false;
    std::vector<Param> params = {};
    std::vector<Param> locals = {};
    /// A source line per instruction, or empty.
    std::vector<std::uint16_t> lines = {};

    std::vector<Instruction> code = {};
    std::map<std::string, std::size_t> labels = {};

    /// Append an instruction.
    FunctionSpec& add(vm::Op op, std::vector<Arg> args = {}) {
        code.push_back({.op = op, .args = std::move(args), .target = {}});
        return *this;
    }
    /// The next instruction is `at` (or the end, if none follows).
    FunctionSpec& label(const std::string& at) {
        labels[at] = code.size();
        return *this;
    }
    FunctionSpec& jmp(std::string to) {
        code.push_back({.op = vm::Op::jmp, .args = {}, .target = std::move(to)});
        return *this;
    }
    FunctionSpec& jmpt(Arg cond, std::string to) {
        code.push_back({.op = vm::Op::jmpt, .args = {std::move(cond)}, .target = std::move(to)});
        return *this;
    }
    FunctionSpec& jmpf(Arg cond, std::string to) {
        code.push_back({.op = vm::Op::jmpf, .args = {std::move(cond)}, .target = std::move(to)});
        return *this;
    }
};

/// A call to a native or another script's function, its result dropped into
/// `::NoneVar` as the compiler does (the function needs that local).
inline void call_static(FunctionSpec& f, const std::string& cls, const std::string& fn, std::vector<Arg> args = {}) {
    std::vector<Arg> all{ident(cls), ident(fn), ident("::NoneVar")};
    all.insert(all.end(), args.begin(), args.end());
    f.add(vm::Op::callstatic, std::move(all));
}

/// A global function with the `::NoneVar` local that calls need for a result
/// nobody reads.
[[nodiscard]] inline FunctionSpec global_function(const std::string& name, std::vector<Param> params = {},
                                                  std::vector<Param> locals = {},
                                                  const std::string& return_type = "None") {
    locals.push_back({"::NoneVar", "None"});
    return {.name = name,
            .return_type = return_type,
            .global = true,
            .params = std::move(params),
            .locals = std::move(locals)};
}

/// A method or event handler in `state` (the default state if empty), with
/// `::NoneVar` likewise.
[[nodiscard]] inline FunctionSpec method(const std::string& name, std::vector<Param> params = {},
                                         std::vector<Param> locals = {}, const std::string& return_type = "None",
                                         const std::string& state = {}) {
    locals.push_back({"::NoneVar", "None"});
    return {.name = name,
            .state = state,
            .return_type = return_type,
            .params = std::move(params),
            .locals = std::move(locals)};
}

/// A function the engine provides: no code, bound with `Vm::bind`.
[[nodiscard]] FunctionSpec native_function(std::string name, std::vector<Param> params = {},
                                           std::string return_type = "None", bool global = true);

struct VariableSpec {
    std::string name;
    std::string type;
    Arg initial = none();
};

struct PropertySpec {
    std::string name;
    std::string type;
    /// An auto property is backed by this variable.
    std::string auto_var = {};
    std::optional<FunctionSpec> getter = {};
    std::optional<FunctionSpec> setter = {};
};

/// One script: a single object, its functions by state.
struct ScriptSpec {
    std::string name;
    std::string parent = {};
    std::string auto_state = {};
    std::uint32_t format_version = 1;
    std::vector<VariableSpec> variables = {};
    std::vector<PropertySpec> properties = {};
    std::vector<FunctionSpec> functions = {};
};

/// The script asset for `spec`.
[[nodiscard]] std::vector<std::uint8_t> build_script(const ScriptSpec& spec);

} // namespace skydot::testing
