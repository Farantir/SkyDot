// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/script_class.hpp"

#include "assets/vpath.hpp"

namespace skydot::vm {

namespace {

/// Fixed argument count per opcode, and whether the three calls add more.
/// Source: bethconv include/bethconv/script/pex.hpp, which every vanilla script
/// decodes with.
constexpr std::uint8_t k_fixed_args[k_op_count] = {
    0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3,
    1, 2, 2, 3, 2, 3, 1, 3, 3, 3, 2, 2, 3, 3, 4, 4,
};

constexpr std::uint8_t k_identifier = 1;
constexpr std::uint8_t k_string = 2;
constexpr std::uint8_t k_bool = 5;

} // namespace

std::string to_lower(std::string_view text) { return ascii_lower(text); }

std::unique_ptr<ScriptClass> ScriptClass::load(std::vector<std::uint8_t> bytes, std::string& error) {
    auto out = std::unique_ptr<ScriptClass>(new ScriptClass());
    out->bytes_ = std::move(bytes);
    flatbuffers::Verifier verifier(out->bytes_.data(), out->bytes_.size());
    if (out->bytes_.empty() || !sfb::VerifyScriptBuffer(verifier)) {
        error = "not a valid script asset";
        return nullptr;
    }
    out->root_ = sfb::GetScript(out->bytes_.data());
    if (out->root_->format_version() != 1) {
        error = "script asset format " + std::to_string(out->root_->format_version()) +
                " is not one this engine reads (it reads 1)";
        return nullptr;
    }
    if (!out->read(error)) {
        return nullptr;
    }
    return out;
}

bool ScriptClass::read(std::string& error) {
    if (const auto* strings = root_->strings()) {
        for (const auto* s : *strings) {
            strings_.push_back(s->str());
            lower_.push_back(to_lower(strings_.back()));
        }
    }
    const auto ok = [&](std::uint32_t index) { return index < strings_.size(); };
    const auto* objects = root_->objects();
    if (objects == nullptr || objects->size() != 1) {
        error = "a script holds exactly one object";
        return false;
    }
    const auto* o = objects->Get(0);
    if (!ok(o->name()) || !ok(o->parent()) || !ok(o->auto_state())) {
        error = "object names outside the string table";
        return false;
    }
    name_ = strings_[o->name()];
    key_ = lower_[o->name()];
    parent_key_ = lower_[o->parent()];
    auto_state_ = lower_[o->auto_state()];

    if (const auto* vars = o->variables()) {
        for (const auto* v : *vars) {
            const auto& initial = v->initial();
            if (!ok(v->name()) || !ok(v->type()) ||
                ((initial.type() == k_identifier || initial.type() == k_string) &&
                 !ok(initial.data()))) {
                error = "variable outside the string table";
                return false;
            }
            variables_.push_back({.name = lower_[v->name()], .type = lower_[v->type()], .initial = initial});
        }
    }
    if (const auto* states = o->states()) {
        for (const auto* s : *states) {
            if (!ok(s->name())) {
                error = "state name outside the string table";
                return false;
            }
            if (s->functions() == nullptr) {
                continue;
            }
            for (const auto* f : *s->functions()) {
                if (!ok(f->name())) {
                    error = "function name outside the string table";
                    return false;
                }
                auto fn = std::make_unique<Function>();
                if (!read_function(*f, lower_[f->name()], *fn, error)) {
                    return false;
                }
                engine_type_ = engine_type_ || fn->native;
                fn->state = lower_[s->name()];
                functions_[lower_[s->name()] + '\x1F' + fn->name] = std::move(fn);
            }
        }
    }
    if (const auto* props = o->properties()) {
        for (const auto* p : *props) {
            if (!ok(p->name()) || !ok(p->type())) {
                error = "property outside the string table";
                return false;
            }
            Property property{.name = lower_[p->name()], .type = lower_[p->type()], .auto_var = {},
                              .getter = nullptr, .setter = nullptr};
            if ((p->flags() & 0x4u) != 0) {
                if (!ok(p->auto_var())) {
                    error = "property variable outside the string table";
                    return false;
                }
                const auto& var = lower_[p->auto_var()];
                for (std::uint32_t i = 0; i < variables_.size(); ++i) {
                    if (variables_[i].name == var) {
                        property.auto_var = i;
                    }
                }
            }
            for (const auto* handler : {p->getter(), p->setter()}) {
                if (handler == nullptr) {
                    continue;
                }
                auto fn = std::make_unique<Function>();
                if (!read_function(*handler, property.name, *fn, error)) {
                    return false;
                }
                fn->handler = handler == p->getter() ? 1 : 2;
                (handler == p->getter() ? property.getter : property.setter) = fn.get();
                handlers_.push_back(std::move(fn));
            }
            properties_[property.name] = std::move(property);
        }
    }
    return true;
}

bool ScriptClass::read_function(const sfb::Function& f, std::string_view name, Function& out,
                                std::string& error) const {
    const auto ok = [&](std::uint32_t index) { return index < strings_.size(); };
    out.owner = this;
    out.name = std::string(name);
    if (!ok(f.return_type())) {
        error = "return type outside the string table";
        return false;
    }
    out.return_type = lower_[f.return_type()];
    out.global = (f.flags() & 0x1u) != 0;
    out.native = (f.flags() & 0x2u) != 0;
    for (const auto* list : {f.params(), f.locals()}) {
        if (list == nullptr) {
            continue;
        }
        for (const auto* nt : *list) {
            if (!ok(nt->name()) || !ok(nt->type())) {
                error = "register outside the string table";
                return false;
            }
            out.reg_names.push_back(lower_[nt->name()]);
            out.reg_types.push_back(lower_[nt->type()]);
        }
        if (list == f.params()) {
            out.param_count = static_cast<std::uint32_t>(out.reg_names.size());
        }
    }
    const std::size_t count = f.opcodes() != nullptr ? f.opcodes()->size() : 0;
    const std::size_t nargs = f.args() != nullptr ? f.args()->size() : 0;
    if (f.arg_offsets() == nullptr ? count != 0 : f.arg_offsets()->size() != count + 1) {
        error = out.name + ": argument offsets do not match the instructions";
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto op = f.opcodes()->Get(static_cast<flatbuffers::uoffset_t>(i));
        const auto begin = f.arg_offsets()->Get(static_cast<flatbuffers::uoffset_t>(i));
        const auto end = f.arg_offsets()->Get(static_cast<flatbuffers::uoffset_t>(i + 1));
        if (op >= k_op_count || begin > end || end > nargs || end - begin < k_fixed_args[op] ||
            (op > 0x19 || op < 0x17 ? end - begin != k_fixed_args[op] : false)) {
            error = out.name + ": malformed instruction " + std::to_string(i);
            return false;
        }
        out.ops.push_back(static_cast<Op>(op));
        out.offsets.push_back(begin);
    }
    out.offsets.push_back(count == 0 ? 0 : f.arg_offsets()->Get(static_cast<flatbuffers::uoffset_t>(count)));
    for (std::size_t a = 0; a < nargs; ++a) {
        const auto* v = f.args()->Get(static_cast<flatbuffers::uoffset_t>(a));
        if (v->type() > k_bool || ((v->type() == k_identifier || v->type() == k_string) && !ok(v->data()))) {
            error = out.name + ": argument outside the string table";
            return false;
        }
        out.args.push_back(*v);
        Slot slot;
        if (v->type() == k_identifier) {
            const auto& id = lower_[v->data()];
            slot = {Slot::Type::name, v->data()};
            for (std::uint32_t r = 0; r < out.reg_names.size(); ++r) {
                if (out.reg_names[r] == id) {
                    slot = {Slot::Type::reg, r};
                    break;
                }
            }
            if (slot.type == Slot::Type::name) {
                for (std::uint32_t i = 0; i < variables_.size(); ++i) {
                    if (variables_[i].name == id) {
                        slot = {Slot::Type::var, i};
                        break;
                    }
                }
            }
            if (slot.type == Slot::Type::name && id == "self") {
                slot = {Slot::Type::self, 0};
            } else if (slot.type == Slot::Type::name && id == "::state") {
                slot = {Slot::Type::state, 0};
            }
        }
        out.slots.push_back(slot);
    }
    // Jumps: the last argument is a relative offset to an instruction or the end.
    for (std::size_t i = 0; i < count; ++i) {
        if (out.ops[i] != Op::jmp && out.ops[i] != Op::jmpt && out.ops[i] != Op::jmpf) {
            continue;
        }
        const auto& offset = out.args[out.offsets[i + 1] - 1];
        const auto target = static_cast<std::int64_t>(i) + static_cast<std::int32_t>(offset.data());
        if (offset.type() != 3 || target < 0 || target > static_cast<std::int64_t>(count)) {
            error = out.name + ": jump at " + std::to_string(i) + " leaves the function";
            return false;
        }
    }
    if (f.lines() != nullptr && f.lines()->size() == count) {
        out.lines.assign(f.lines()->begin(), f.lines()->end());
    }
    return true;
}

const Function* ScriptClass::find(std::string_view state, std::string_view name) const {
    std::string key;
    key.reserve(state.size() + 1 + name.size());
    key.append(state).push_back('\x1F');
    key.append(name);
    const auto it = functions_.find(key);
    return it != functions_.end() ? it->second.get() : nullptr;
}

const Property* ScriptClass::property(std::string_view name) const {
    const auto it = properties_.find(name);
    return it != properties_.end() ? &it->second : nullptr;
}

bool ScriptClass::derives_from(const ScriptClass* other) const noexcept {
    for (const auto* c = this; c != nullptr; c = c->parent_) {
        if (c == other) {
            return true;
        }
    }
    return false;
}

} // namespace skydot::vm
