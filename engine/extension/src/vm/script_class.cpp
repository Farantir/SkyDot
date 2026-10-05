// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/script_class.hpp"

#include "assets/vpath.hpp"
#include "skydot_formats/flags.hpp"

#include <bit>

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

TypeKind type_kind(std::string_view type) {
    if (type == "int") {
        return TypeKind::integer;
    }
    if (type == "float") {
        return TypeKind::floating;
    }
    if (type == "bool") {
        return TypeKind::boolean;
    }
    if (type == "string") {
        return TypeKind::string;
    }
    if (type.ends_with("[]")) {
        return TypeKind::array;
    }
    return type == "none" ? TypeKind::none : TypeKind::object;
}

Value default_of(TypeKind kind) {
    switch (kind) {
    case TypeKind::integer: return Value::integer(0);
    case TypeKind::floating: return Value::floating(0.0F);
    case TypeKind::boolean: return Value::boolean(false);
    case TypeKind::string: {
        static const auto k_empty = std::make_shared<const std::string>();
        return Value::text(k_empty);
    }
    default: return {};
    }
}

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
        strings_.reserve(strings->size());
        lower_.reserve(strings->size());
        lower_hash_.reserve(strings->size());
        for (const auto* s : *strings) {
            strings_.push_back(s->str());
            lower_.push_back(to_lower(strings_.back()));
            lower_hash_.push_back(name_hash(lower_.back()));
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
            variables_.push_back({.name = lower_[v->name()], .type = lower_[v->type()], .initial = initial,
                                  .kind = type_kind(lower_[v->type()])});
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
                const Name state = lower_name(s->name());
                const Name name = lower_name(f->name());
                function_table_.put(function_hash(state, name), fn.get(), [&](const Function& other) {
                    return other.state == state.text && other.name == name.text;
                });
                functions_.push_back(std::move(fn));
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
                              .getter = nullptr, .setter = nullptr, .kind = type_kind(lower_[p->type()])};
            if (formats::has_flag(p->flags(), sfb::PropertyFlags::is_auto)) {
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
            const auto& stored = (properties_[property.name] = std::move(property));
            property_table_.put(name_hash(stored.name), &stored,
                                [&](const Property& other) { return other.name == stored.name; });
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
    out.global = formats::has_flag(f.flags(), sfb::FunctionFlags::global);
    out.native = formats::has_flag(f.flags(), sfb::FunctionFlags::native);
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
            out.reg_kinds.push_back(type_kind(out.reg_types.back()));
            out.reg_defaults.push_back(default_of(out.reg_kinds.back()));
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
        // A failing callstatic prints both names from the string table, so
        // they must be names, not literals whose value would index it.
        if (static_cast<Op>(op) == Op::callstatic) {
            for (std::uint32_t n = begin; n < begin + 2; ++n) {
                const auto type = f.args()->Get(n)->type();
                if (type != k_identifier && type != k_string) {
                    error = out.name + ": instruction " + std::to_string(i) +
                            " names its class or function by a literal";
                    return false;
                }
            }
        }
        out.ops.push_back(static_cast<Op>(op));
        out.offsets.push_back(begin);
    }
    out.offsets.push_back(count == 0 ? 0 : f.arg_offsets()->Get(static_cast<flatbuffers::uoffset_t>(count)));
    // An identifier resolves once per function, however often it is used.
    std::vector<Slot> resolved(strings_.size());
    std::vector<bool> is_resolved(strings_.size(), false);
    for (std::size_t a = 0; a < nargs; ++a) {
        const auto* v = f.args()->Get(static_cast<flatbuffers::uoffset_t>(a));
        if (v->type() > k_bool || ((v->type() == k_identifier || v->type() == k_string) && !ok(v->data()))) {
            error = out.name + ": argument outside the string table";
            return false;
        }
        out.args.push_back(*v);
        Slot slot;
        if (v->type() == k_identifier && is_resolved[v->data()]) {
            slot = resolved[v->data()];
        } else if (v->type() == k_identifier) {
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
            resolved[v->data()] = slot;
            is_resolved[v->data()] = true;
        }
        if (v->type() != k_identifier) {
            slot = {Slot::Type::literal, static_cast<std::uint32_t>(out.literals.size())};
            out.literals.push_back(literal(*v));
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

Value ScriptClass::literal(const sfb::Value& v) const {
    switch (v.type()) {
    case k_string: return Value::string(strings_[v.data()]);
    case 3: return Value::integer(static_cast<std::int32_t>(v.data()));
    case 4: return Value::floating(std::bit_cast<float>(v.data()));
    case k_bool: return Value::boolean(v.data() != 0);
    default: return {};
    }
}

const Function* ScriptClass::find(const Name& state, const Name& name) const {
    return function_table_.find(function_hash(state, name), [&](const Function& f) {
        return f.name == name.text && f.state == state.text;
    });
}

const Property* ScriptClass::property(const Name& name) const {
    return property_table_.find(name.hash, [&](const Property& p) { return p.name == name.text; });
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
