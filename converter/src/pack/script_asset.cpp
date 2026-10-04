// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/pack/script_asset.hpp"

#include "bethconv/io/byte_view.hpp"
#include "bethconv/io/span_reader.hpp"

#include "bethconv/pack/script_generated.h"

#include <utility>

namespace bethconv::pack {

namespace {

namespace sfb = bethconv::pack::sfb;
using script::PexFunction;
using script::PexScript;

std::vector<sfb::NameType> name_types(const std::vector<script::PexNameType>& list) {
    std::vector<sfb::NameType> out;
    out.reserve(list.size());
    for (const auto& e : list) {
        out.emplace_back(e.name, e.type);
    }
    return out;
}

flatbuffers::Offset<sfb::Function> write_function(flatbuffers::FlatBufferBuilder& b,
                                                  const PexFunction& f) {
    std::vector<std::uint8_t> opcodes;
    opcodes.reserve(f.opcodes.size());
    for (const auto op : f.opcodes) {
        opcodes.push_back(static_cast<std::uint8_t>(op));
    }
    std::vector<sfb::Value> args;
    args.reserve(f.args.size());
    for (const auto& a : f.args) {
        args.emplace_back(static_cast<std::uint8_t>(a.type), a.data);
    }
    const auto params = b.CreateVectorOfStructs(name_types(f.params));
    const auto locals = b.CreateVectorOfStructs(name_types(f.locals));
    const auto ops = b.CreateVector(opcodes);
    const auto offsets = b.CreateVector(f.arg_offsets);
    const auto args_off = b.CreateVectorOfStructs(args);
    const auto lines = b.CreateVector(f.lines);
    return sfb::CreateFunction(b, f.name, f.return_type, f.user_flags, f.flags, params, locals, ops,
                               offsets, args_off, lines);
}

PexFunction read_function(const sfb::Function& f) {
    PexFunction out;
    out.name = f.name();
    out.return_type = f.return_type();
    out.user_flags = f.user_flags();
    out.flags = f.flags();
    const auto list = [](const flatbuffers::Vector<const sfb::NameType*>* v) {
        std::vector<script::PexNameType> r;
        if (v != nullptr) {
            for (const auto* e : *v) {
                r.push_back({.name = e->name(), .type = e->type()});
            }
        }
        return r;
    };
    out.params = list(f.params());
    out.locals = list(f.locals());
    if (f.opcodes() != nullptr) {
        for (const auto op : *f.opcodes()) {
            out.opcodes.push_back(static_cast<script::PexOp>(op));
        }
    }
    if (f.arg_offsets() != nullptr) {
        out.arg_offsets.assign(f.arg_offsets()->begin(), f.arg_offsets()->end());
    }
    if (f.args() != nullptr) {
        for (const auto* a : *f.args()) {
            out.args.push_back({.type = static_cast<script::PexValueType>(a->type()), .data = a->data()});
        }
    }
    if (f.lines() != nullptr) {
        out.lines.assign(f.lines()->begin(), f.lines()->end());
    }
    return out;
}

} // namespace

std::vector<std::byte> write_script_asset(const PexScript& script) {
    flatbuffers::FlatBufferBuilder b(4096);
    std::vector<flatbuffers::Offset<sfb::Object>> objects;
    for (const auto& o : script.objects) {
        std::vector<sfb::Variable> variables;
        for (const auto& v : o.variables) {
            variables.emplace_back(v.name, v.type, v.user_flags,
                                   sfb::Value(static_cast<std::uint8_t>(v.initial.type), v.initial.data));
        }
        std::vector<flatbuffers::Offset<sfb::Property>> properties;
        for (const auto& p : o.properties) {
            const auto getter = p.getter ? write_function(b, *p.getter) : 0;
            const auto setter = p.setter ? write_function(b, *p.setter) : 0;
            properties.push_back(sfb::CreateProperty(b, p.name, p.type, p.user_flags, p.flags,
                                                     p.auto_var, getter, setter));
        }
        std::vector<flatbuffers::Offset<sfb::State>> states;
        for (const auto& s : o.states) {
            std::vector<flatbuffers::Offset<sfb::Function>> functions;
            for (const auto& f : s.functions) {
                functions.push_back(write_function(b, f));
            }
            states.push_back(sfb::CreateState(b, s.name, b.CreateVector(functions)));
        }
        const auto variables_off = b.CreateVectorOfStructs(variables);
        const auto properties_off = b.CreateVector(properties);
        const auto states_off = b.CreateVector(states);
        objects.push_back(sfb::CreateObject(b, o.name, o.parent, o.user_flags, o.auto_state,
                                            variables_off, properties_off, states_off));
    }
    std::vector<flatbuffers::Offset<flatbuffers::String>> strings;
    strings.reserve(script.strings.size());
    for (const auto& s : script.strings) {
        strings.push_back(b.CreateString(s));
    }
    std::vector<sfb::UserFlag> flags;
    for (const auto& f : script.user_flags) {
        flags.emplace_back(f.name, f.bit);
    }
    const auto source = b.CreateString(script.source_file);
    const auto strings_off = b.CreateVector(strings);
    const auto flags_off = b.CreateVectorOfStructs(flags);
    const auto objects_off = b.CreateVector(objects);
    sfb::FinishScriptBuffer(b, sfb::CreateScript(b, k_script_format_version, source, strings_off,
                                                 flags_off, objects_off));
    const std::span<const std::uint8_t> raw(b.GetBufferPointer(), b.GetSize());
    const auto bytes = std::as_bytes(raw);
    return {bytes.begin(), bytes.end()};
}

io::ParseResult<PexScript> read_script_asset(std::span<const std::byte> bytes,
                                             std::string_view origin) {
    io::SpanReader reader(bytes, origin);
    const auto* raw = io::as_u8(bytes).data();
    flatbuffers::Verifier verifier(raw, bytes.size());
    if (bytes.empty() || !sfb::VerifyScriptBuffer(verifier)) {
        return reader.fail(io::ErrorKind::corrupt, "not a valid script asset");
    }
    const auto* root = sfb::GetScript(raw);
    if (root->format_version() != k_script_format_version) {
        return reader.fail(io::ErrorKind::unsupported,
                           "script asset format " + std::to_string(root->format_version()) +
                               " is not one this build reads (it reads " +
                               std::to_string(k_script_format_version) + ")");
    }
    PexScript out;
    if (root->source() != nullptr) {
        out.source_file = root->source()->str();
    }
    if (root->strings() != nullptr) {
        for (const auto* s : *root->strings()) {
            out.strings.push_back(s->str());
        }
    }
    if (root->user_flags() != nullptr) {
        for (const auto* f : *root->user_flags()) {
            out.user_flags.push_back({.name = f->name(), .bit = f->bit()});
        }
    }
    if (root->objects() != nullptr) {
        for (const auto* o : *root->objects()) {
            script::PexObject object;
            object.name = o->name();
            object.parent = o->parent();
            object.user_flags = o->user_flags();
            object.auto_state = o->auto_state();
            if (o->variables() != nullptr) {
                for (const auto* v : *o->variables()) {
                    object.variables.push_back(
                        {.name = v->name(),
                         .type = v->type(),
                         .user_flags = v->user_flags(),
                         .initial = {.type = static_cast<script::PexValueType>(v->initial().type()),
                                     .data = v->initial().data()}});
                }
            }
            if (o->properties() != nullptr) {
                for (const auto* p : *o->properties()) {
                    script::PexProperty property;
                    property.name = p->name();
                    property.type = p->type();
                    property.user_flags = p->user_flags();
                    property.flags = p->flags();
                    property.auto_var = p->auto_var();
                    if (p->getter() != nullptr) {
                        property.getter = read_function(*p->getter());
                    }
                    if (p->setter() != nullptr) {
                        property.setter = read_function(*p->setter());
                    }
                    object.properties.push_back(std::move(property));
                }
            }
            if (o->states() != nullptr) {
                for (const auto* s : *o->states()) {
                    script::PexState state;
                    state.name = s->name();
                    if (s->functions() != nullptr) {
                        for (const auto* f : *s->functions()) {
                            state.functions.push_back(read_function(*f));
                        }
                    }
                    object.states.push_back(std::move(state));
                }
            }
            out.objects.push_back(std::move(object));
        }
    }
    return out;
}

} // namespace bethconv::pack
