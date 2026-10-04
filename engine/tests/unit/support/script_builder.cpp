// SPDX-License-Identifier: GPL-3.0-or-later
#include "script_builder.hpp"

#include <bit>
#include <stdexcept>

namespace skydot::testing {

namespace sfb = vm::sfb;

Arg floating(float v) {
    return {.type = 4, .text = {}, .bits = std::bit_cast<std::uint32_t>(v)};
}

FunctionSpec native_function(std::string name, std::vector<Param> params, std::string return_type,
                             bool global) {
    return {.name = std::move(name),
            .state = {},
            .return_type = std::move(return_type),
            .global = global,
            .native = true,
            .params = std::move(params)};
}

namespace {

/// The string table, which deduplicates as the PEX's does.
class Strings {
public:
    Strings() { index(""); } // parent and auto state name the empty string for none
    std::uint16_t index(const std::string& text) {
        for (std::size_t i = 0; i < table_.size(); ++i) {
            if (table_[i] == text) {
                return static_cast<std::uint16_t>(i);
            }
        }
        table_.push_back(text);
        return static_cast<std::uint16_t>(table_.size() - 1);
    }
    [[nodiscard]] const std::vector<std::string>& all() const { return table_; }

private:
    std::vector<std::string> table_;
};

sfb::Value value(Strings& strings, const Arg& a) {
    if (a.type == 1 || a.type == 2) {
        return sfb::Value(a.type, a.raw ? a.bits : strings.index(a.text));
    }
    return sfb::Value(a.type, a.bits);
}

flatbuffers::Offset<sfb::Function> function(flatbuffers::FlatBufferBuilder& fbb, Strings& strings,
                                            const FunctionSpec& f, const std::string& name) {
    std::vector<sfb::NameType> params;
    for (const auto& p : f.params) {
        params.emplace_back(strings.index(p.name), strings.index(p.type));
    }
    std::vector<sfb::NameType> locals;
    for (const auto& l : f.locals) {
        locals.emplace_back(strings.index(l.name), strings.index(l.type));
    }

    std::vector<std::uint8_t> ops;
    std::vector<std::uint32_t> offsets{0};
    std::vector<sfb::Value> args;
    for (std::size_t i = 0; i < f.code.size(); ++i) {
        const auto& ins = f.code[i];
        ops.push_back(static_cast<std::uint8_t>(ins.op));
        for (const auto& a : ins.args) {
            args.push_back(value(strings, a));
        }
        if (!ins.target.empty()) {
            const auto label = f.labels.find(ins.target);
            if (label == f.labels.end()) {
                throw std::logic_error("test script: no label " + ins.target);
            }
            const auto relative = static_cast<std::int64_t>(label->second) - static_cast<std::int64_t>(i);
            args.push_back(value(strings, integer(static_cast<std::int32_t>(relative))));
        }
        offsets.push_back(static_cast<std::uint32_t>(args.size()));
    }
    auto flags = sfb::FunctionFlags::NONE;
    if (f.global) {
        flags |= sfb::FunctionFlags::global;
    }
    if (f.native) {
        flags |= sfb::FunctionFlags::native;
    }
    return sfb::CreateFunctionDirect(fbb, strings.index(name), strings.index(f.return_type), 0, flags,
                                     &params, &locals, &ops, &offsets, &args,
                                     f.lines.empty() ? nullptr : &f.lines);
}

} // namespace

std::vector<std::uint8_t> build_script(const ScriptSpec& spec) {
    flatbuffers::FlatBufferBuilder fbb;
    Strings strings;

    // Functions grouped by state, in the order a state first appears.
    std::vector<std::string> state_names;
    std::map<std::string, std::vector<flatbuffers::Offset<sfb::Function>>> by_state;
    for (const auto& f : spec.functions) {
        if (!by_state.contains(f.state)) {
            state_names.push_back(f.state);
        }
        by_state[f.state].push_back(function(fbb, strings, f, f.name));
    }
    std::vector<flatbuffers::Offset<sfb::State>> states;
    for (const auto& s : state_names) {
        states.push_back(sfb::CreateStateDirect(fbb, strings.index(s), &by_state[s]));
    }

    std::vector<sfb::Variable> variables;
    for (const auto& v : spec.variables) {
        variables.emplace_back(strings.index(v.name), strings.index(v.type), 0, value(strings, v.initial));
    }

    std::vector<flatbuffers::Offset<sfb::Property>> properties;
    for (const auto& p : spec.properties) {
        flatbuffers::Offset<sfb::Function> getter;
        flatbuffers::Offset<sfb::Function> setter;
        if (p.getter) {
            getter = function(fbb, strings, *p.getter, p.name);
        }
        if (p.setter) {
            setter = function(fbb, strings, *p.setter, p.name);
        }
        auto flags = sfb::PropertyFlags::NONE;
        if (p.getter) {
            flags |= sfb::PropertyFlags::has_getter;
        }
        if (p.setter) {
            flags |= sfb::PropertyFlags::has_setter;
        }
        if (!p.auto_var.empty()) {
            flags |= sfb::PropertyFlags::is_auto;
        }
        properties.push_back(sfb::CreateProperty(
            fbb, strings.index(p.name), strings.index(p.type), 0, flags,
            p.auto_var.empty() ? std::uint16_t{0} : strings.index(p.auto_var), getter, setter));
    }

    const auto object = sfb::CreateObjectDirect(fbb, strings.index(spec.name), strings.index(spec.parent), 0,
                                                strings.index(spec.auto_state), &variables, &properties,
                                                &states);
    std::vector<flatbuffers::Offset<sfb::Object>> objects{object};

    std::vector<flatbuffers::Offset<flatbuffers::String>> table;
    for (const auto& s : strings.all()) {
        table.push_back(fbb.CreateString(s));
    }
    sfb::FinishScriptBuffer(
        fbb, sfb::CreateScriptDirect(fbb, spec.format_version, "test.psc", &table, nullptr, &objects));
    return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

} // namespace skydot::testing
