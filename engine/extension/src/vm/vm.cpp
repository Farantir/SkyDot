// SPDX-License-Identifier: GPL-3.0-or-later
#include "vm/vm.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace skydot::vm {

namespace {

constexpr std::size_t k_max_depth = 512;
/// Papyrus arrays hold at most 128 elements.
constexpr std::int32_t k_max_array = 128;

std::int32_t as_int(const Value& v) {
    switch (v.kind) {
    case Kind::integer: return v.i;
    case Kind::floating: return static_cast<std::int32_t>(v.f);
    case Kind::boolean: return v.b ? 1 : 0;
    default: return 0;
    }
}

float as_float(const Value& v) {
    switch (v.kind) {
    case Kind::integer: return static_cast<float>(v.i);
    case Kind::floating: return v.f;
    case Kind::boolean: return v.b ? 1.0F : 0.0F;
    default: return 0.0F;
    }
}

// Papyrus ints are 32-bit two's complement and wrap, as in the game's native
// VM. Signed overflow is undefined in C++ (and INT_MIN / -1 traps on x86), so
// the arithmetic runs on the unsigned bits.
std::int32_t wrap(std::uint32_t bits) { return static_cast<std::int32_t>(bits); }

std::int32_t int_add(std::int32_t x, std::int32_t y) {
    return wrap(static_cast<std::uint32_t>(x) + static_cast<std::uint32_t>(y));
}

std::int32_t int_sub(std::int32_t x, std::int32_t y) {
    return wrap(static_cast<std::uint32_t>(x) - static_cast<std::uint32_t>(y));
}

std::int32_t int_mul(std::int32_t x, std::int32_t y) {
    return wrap(static_cast<std::uint32_t>(x) * static_cast<std::uint32_t>(y));
}

std::int32_t int_neg(std::int32_t x) { return wrap(0U - static_cast<std::uint32_t>(x)); }

/// `y` is not 0. Dividing by -1 is a negation (INT_MIN stays INT_MIN) and
/// leaves no remainder.
std::int32_t int_div(std::int32_t x, std::int32_t y) { return y == -1 ? int_neg(x) : x / y; }
std::int32_t int_mod(std::int32_t x, std::int32_t y) { return y == -1 ? 0 : x % y; }

bool is_number(const Value& v) { return v.kind == Kind::integer || v.kind == Kind::floating; }

int compare_text(std::string_view a, std::string_view b) {
    const auto n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto x = static_cast<unsigned char>(a[i] >= 'A' && a[i] <= 'Z' ? a[i] - 'A' + 'a' : a[i]);
        const auto y = static_cast<unsigned char>(b[i] >= 'A' && b[i] <= 'Z' ? b[i] - 'A' + 'a' : b[i]);
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

bool equals(const Value& a, const Value& b) {
    if (is_number(a) && is_number(b)) {
        return a.kind == Kind::integer && b.kind == Kind::integer ? a.i == b.i
                                                                   : as_float(a) == as_float(b);
    }
    if (a.kind != b.kind) {
        return false;
    }
    switch (a.kind) {
    case Kind::none: return true;
    case Kind::boolean: return a.b == b.b;
    case Kind::string: return compare_text(a.s, b.s) == 0;
    case Kind::object: return a.form == b.form;
    case Kind::array: return a.array == b.array;
    default: return false;
    }
}

/// -1, 0 or 1; strings compare without case, everything else as numbers.
int order(const Value& a, const Value& b) {
    if (a.kind == Kind::string && b.kind == Kind::string) {
        return compare_text(a.s, b.s);
    }
    if (a.kind == Kind::integer && b.kind == Kind::integer) {
        return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
    }
    const float x = as_float(a);
    const float y = as_float(b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

Value literal(const ScriptClass& owner, const sfb::Value& v) {
    switch (v.type()) {
    case 2: return Value::string(owner.string(v.data()));
    case 3: return Value::integer(static_cast<std::int32_t>(v.data()));
    case 4: return Value::floating(std::bit_cast<float>(v.data()));
    case 5: return Value::boolean(v.data() != 0);
    default: return {};
    }
}

bool starts_with_on(std::string_view name) { return name.size() > 2 && name.starts_with("on"); }

} // namespace

std::vector<Value>* Instance::vars_of(const ScriptClass* c) {
    for (auto& [owner, values] : vars) {
        if (owner == c) {
            return &values;
        }
    }
    return nullptr;
}

// ---- classes and instances ------------------------------------------------

void Vm::bind(std::string_view cls, std::string_view fn, Native native) {
    natives_[to_lower(cls) + "." + to_lower(fn)] = std::move(native);
}

void Vm::error(const std::string& message) {
    ++errors_;
    if (log_) {
        log_(message);
    }
}

const ScriptClass* Vm::load_class(std::string_view name) {
    const std::string key = to_lower(name);
    if (const auto it = classes_.find(key); it != classes_.end()) {
        return it->second.get();
    }
    if (key.empty() || missing_.contains(key)) {
        return nullptr;
    }
    // Mark first, so a class that names itself as an ancestor ends the walk.
    missing_.insert(key);
    auto bytes = loader_ ? loader_(key) : std::nullopt;
    if (!bytes) {
        error("script " + std::string(name) + " is not in the pack");
        return nullptr;
    }
    std::string why;
    auto cls = ScriptClass::load(std::move(*bytes), why);
    if (cls == nullptr) {
        error("script " + std::string(name) + ": " + why);
        return nullptr;
    }
    if (cls->key() != key) {
        error("script " + std::string(name) + " holds class " + cls->name());
        return nullptr;
    }
    if (!cls->parent_key().empty()) {
        const auto* parent = load_class(cls->parent_key());
        if (parent == nullptr) {
            error("script " + cls->name() + ": parent " + cls->parent_key() + " does not load");
            return nullptr;
        }
        cls->set_parent(parent);
    }
    missing_.erase(key);
    return classes_.emplace(key, std::move(cls)).first->second.get();
}

Instance* Vm::attach(std::uint32_t form, std::string_view class_name) {
    const auto* cls = load_class(class_name);
    if (cls == nullptr) {
        return nullptr;
    }
    if (cls->engine_type()) {
        error("script " + cls->name() + " is an engine type and cannot be attached");
        return nullptr;
    }
    auto instance = std::make_unique<Instance>();
    instance->cls = cls;
    instance->form = form;
    instance->state = cls->auto_state();
    for (const auto* c = cls; c != nullptr; c = c->parent()) {
        std::vector<Value> values;
        for (const auto& v : c->variables()) {
            Value initial = literal(*c, v.initial);
            values.push_back(initial.is_none() ? default_for(v.type) : cast(initial, v.type));
        }
        instance->vars.emplace_back(c, std::move(values));
    }
    auto& list = instances_[form];
    list.push_back(std::move(instance));
    return list.back().get();
}

std::vector<Instance*> Vm::instances(std::uint32_t form) const {
    std::vector<Instance*> out;
    if (const auto it = instances_.find(form); it != instances_.end()) {
        for (const auto& i : it->second) {
            out.push_back(i.get());
        }
    }
    return out;
}

Instance* Vm::instance(std::uint32_t form, std::string_view class_name) const {
    const auto key = to_lower(class_name);
    for (auto* i : instances(form)) {
        if (i->cls->key() == key) {
            return i;
        }
    }
    return nullptr;
}

Value Vm::object(std::uint32_t form, std::string_view cls) {
    return Value::object(form, load_class(cls));
}

bool Vm::set_property(Instance* instance, std::string_view name, Value value) {
    const auto key = to_lower(name);
    for (const auto* c = instance->cls; c != nullptr; c = c->parent()) {
        const auto* p = c->property(key);
        if (p == nullptr) {
            continue;
        }
        if (p->auto_var) {
            (*instance->vars_of(c))[*p->auto_var] = cast(value, p->type);
            return true;
        }
        if (p->setter != nullptr) {
            start({p->setter, instance}, Value::object(instance->form, instance->cls, instance),
                  {std::move(value)}, false);
            return true;
        }
        return false;
    }
    return false;
}

Value Vm::get_variable(Instance* instance, std::string_view name) const {
    const auto key = to_lower(name);
    for (auto& [owner, values] : instance->vars) {
        const auto& vars = owner->variables();
        for (std::size_t i = 0; i < vars.size(); ++i) {
            if (vars[i].name == key) {
                return values[i];
            }
        }
    }
    return {};
}

// ---- starting threads -----------------------------------------------------

const Function* Vm::find_in_chain(const ScriptClass* start, std::string_view state,
                                  std::string_view name) {
    for (const auto* c = start; c != nullptr; c = c->parent()) {
        if (!state.empty()) {
            if (const auto* f = c->find(state, name)) {
                return f;
            }
        }
        if (const auto* f = c->find("", name)) {
            return f;
        }
    }
    return nullptr;
}

Vm::Found Vm::find_method(const Value& object, std::string_view name) {
    const auto try_instance = [&](Instance* i) -> Found {
        if (i == nullptr || (object.cls != nullptr && !i->cls->derives_from(object.cls))) {
            return {};
        }
        return {find_in_chain(i->cls, to_lower(i->state), name), i};
    };
    if (auto found = try_instance(object.instance); found.fn != nullptr) {
        return found;
    }
    for (auto* i : instances(object.form)) {
        if (auto found = try_instance(i); found.fn != nullptr) {
            return found;
        }
    }
    return {find_in_chain(object.cls, "", name), nullptr};
}

std::uint64_t Vm::start(Found found, Value self, std::vector<Value> args, bool keep_result) {
    if (found.fn == nullptr) {
        return 0;
    }
    auto thread = std::make_unique<Thread>();
    thread->id = next_thread_++;
    thread->keep_result = keep_result;
    const auto id = thread->id;
    if (found.fn->native) {
        std::vector<Value> converted = std::move(args);
        NativeCall call{*this, *thread, std::move(self), converted};
        const auto key = found.fn->owner->key() + "." + found.fn->name;
        const auto native = natives_.find(key);
        const NativeResult result = native != natives_.end() ? native->second(call) : NativeResult{};
        if (keep_result) {
            results_[id] = result.value;
        }
        return id;
    }
    if (!push(*thread, found.fn, found.instance, std::move(self), std::move(args), -1)) {
        return 0;
    }
    threads_.push_back(std::move(thread));
    return id;
}

std::size_t Vm::send_event(std::uint32_t form, std::string_view event, std::vector<Value> args) {
    const auto key = to_lower(event);
    std::size_t started = 0;
    for (auto* i : instances(form)) {
        const auto* fn = find_in_chain(i->cls, to_lower(i->state), key);
        if (fn == nullptr || fn->native) {
            continue;
        }
        if (start({fn, i}, Value::object(form, i->cls, i), args, false) != 0) {
            ++started;
        }
    }
    return started;
}

std::uint64_t Vm::call_global(std::string_view cls, std::string_view fn, std::vector<Value> args) {
    const auto* c = load_class(cls);
    if (c == nullptr) {
        return 0;
    }
    return start({find_in_chain(c, "", to_lower(fn)), nullptr}, Value{}, std::move(args), true);
}

std::uint64_t Vm::call_method(Instance* instance, std::string_view fn, std::vector<Value> args) {
    if (instance == nullptr) {
        return 0;
    }
    const Value self = Value::object(instance->form, instance->cls, instance);
    return start(find_method(self, to_lower(fn)), self, std::move(args), true);
}

std::optional<Value> Vm::take_result(std::uint64_t thread) {
    const auto it = results_.find(thread);
    if (it == results_.end()) {
        return std::nullopt;
    }
    Value out = std::move(it->second);
    results_.erase(it);
    return out;
}

void Vm::notify(std::string_view key) {
    const auto lower = to_lower(key);
    for (auto& t : threads_) {
        if (!t->done && !t->wait_for.empty() && t->wait_for == lower) {
            t->wait_for.clear();
            t->wake_at = time_;
        }
    }
}

void Vm::register_update(Instance* instance, double delay, double interval) {
    unregister_update(instance);
    timers_.push_back({instance, time_ + std::max(0.0, delay), interval});
}

void Vm::unregister_update(Instance* instance) {
    std::erase_if(timers_, [&](const Timer& t) { return t.instance == instance; });
}

std::uint32_t Vm::random() {
    // splitmix64: small, seeded, and its whole state saves as one number.
    rng_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = rng_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return static_cast<std::uint32_t>((z ^ (z >> 31)) >> 32);
}

void Vm::fire_timers() {
    std::vector<Instance*> due;
    for (auto it = timers_.begin(); it != timers_.end();) {
        if (it->due > time_) {
            ++it;
            continue;
        }
        due.push_back(it->instance);
        if (it->interval > 0.0) {
            it->due += it->interval * std::max(1.0, std::ceil((time_ - it->due) / it->interval));
            ++it;
        } else {
            it = timers_.erase(it);
        }
    }
    for (auto* i : due) {
        const auto* fn = find_in_chain(i->cls, to_lower(i->state), "onupdate");
        if (fn != nullptr && !fn->native) {
            start({fn, i}, Value::object(i->form, i->cls, i), {}, false);
        }
    }
}

void Vm::clear() {
    threads_.clear();
    results_.clear();
    timers_.clear();
    instances_.clear();
}

void Vm::update(double seconds) {
    time_ += seconds;
    fire_timers();
    // Threads started while running are appended and run in this update too.
    for (std::size_t i = 0; i < threads_.size(); ++i) {
        auto& t = *threads_[i];
        if (!t.done && t.wake_at <= time_) {
            t.wait_for.clear(); // woken, or timed out
            run(t);
        }
    }
    std::erase_if(threads_, [&](const std::unique_ptr<Thread>& t) {
        if (t->done && t->keep_result) {
            results_[t->id] = t->result;
        }
        return t->done;
    });
    // Results nobody takes (events, setters) should not pile up.
    if (results_.size() > 4096) {
        results_.clear();
    }
}

// ---- conversions ----------------------------------------------------------

Value Vm::default_for(std::string_view type) {
    if (type == "int") {
        return Value::integer(0);
    }
    if (type == "float") {
        return Value::floating(0.0F);
    }
    if (type == "bool") {
        return Value::boolean(false);
    }
    if (type == "string") {
        return Value::string("");
    }
    return {};
}

bool Vm::truthy(const Value& v) {
    switch (v.kind) {
    case Kind::none: return false;
    case Kind::integer: return v.i != 0;
    case Kind::floating: return v.f != 0.0F;
    case Kind::boolean: return v.b;
    case Kind::string: return !v.s.empty();
    case Kind::object: return true;
    case Kind::array: return !v.array->empty();
    }
    return false;
}

std::string Vm::to_string(const Value& v) {
    switch (v.kind) {
    case Kind::none: return "None";
    case Kind::integer: return std::to_string(v.i);
    case Kind::floating: {
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "%f", static_cast<double>(v.f));
        return buffer;
    }
    case Kind::boolean: return v.b ? "True" : "False";
    case Kind::string: return v.s;
    case Kind::object: {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%08X", v.form);
        return "[" + (v.cls != nullptr ? v.cls->name() : std::string("Form")) + " <" + buffer + ">]";
    }
    case Kind::array: {
        std::string out = "[";
        for (std::size_t i = 0; i < v.array->size(); ++i) {
            out += (i != 0 ? ", " : "") + to_string((*v.array)[i]);
        }
        return out + "]";
    }
    }
    return {};
}

Value Vm::cast(const Value& v, std::string_view type) {
    if (type == "int") {
        if (v.kind == Kind::string) {
            return Value::integer(static_cast<std::int32_t>(std::strtol(v.s.c_str(), nullptr, 10)));
        }
        return Value::integer(as_int(v));
    }
    if (type == "float") {
        if (v.kind == Kind::string) {
            return Value::floating(std::strtof(v.s.c_str(), nullptr));
        }
        return Value::floating(as_float(v));
    }
    if (type == "bool") {
        return Value::boolean(truthy(v));
    }
    if (type == "string") {
        return Value::string(to_string(v));
    }
    if (type.ends_with("[]")) {
        return v.kind == Kind::array ? v : Value{};
    }
    if (type == "none" || v.kind != Kind::object) {
        return v.kind == Kind::none || type == "none" ? v : Value{};
    }
    const auto* target = load_class(type);
    if (target == nullptr) {
        return {};
    }
    if (v.instance != nullptr && v.instance->cls->derives_from(target)) {
        return Value::object(v.form, target, v.instance);
    }
    for (auto* i : instances(v.form)) {
        if (i->cls->derives_from(target)) {
            return Value::object(v.form, target, i);
        }
    }
    // An engine type, or an upcast: the form itself.
    if (target->engine_type() || (v.cls != nullptr && v.cls->derives_from(target))) {
        return Value::object(v.form, target, nullptr);
    }
    return {};
}

// ---- the interpreter ------------------------------------------------------

std::string Vm::where(const Frame& frame) const {
    std::string out = frame.fn->owner->name() + "." + frame.fn->name;
    if (frame.pc < frame.fn->lines.size()) {
        out += " line " + std::to_string(frame.fn->lines[frame.pc]);
    }
    return out;
}

Value Vm::read(Frame& frame, std::uint32_t arg) {
    const Function& fn = *frame.fn;
    const auto& v = fn.args[arg];
    if (v.type() != 1) {
        return literal(*fn.owner, v);
    }
    const Slot& slot = fn.slots[arg];
    switch (slot.type) {
    case Slot::Type::reg: return frame.regs[slot.index];
    case Slot::Type::var:
        if (auto* vars = frame.instance != nullptr ? frame.instance->vars_of(fn.owner) : nullptr) {
            return (*vars)[slot.index];
        }
        break;
    case Slot::Type::self: return frame.self;
    case Slot::Type::state:
        return Value::string(frame.instance != nullptr ? frame.instance->state : std::string{});
    default: break;
    }
    error(where(frame) + ": cannot read " + fn.owner->string(v.data()));
    return {};
}

void Vm::write(Frame& frame, std::uint32_t arg, Value value) {
    const Function& fn = *frame.fn;
    const auto& v = fn.args[arg];
    const Slot& slot = fn.slots[arg];
    if (v.type() == 1) {
        switch (slot.type) {
        case Slot::Type::reg: frame.regs[slot.index] = std::move(value); return;
        case Slot::Type::var:
            if (auto* vars = frame.instance != nullptr ? frame.instance->vars_of(fn.owner) : nullptr) {
                (*vars)[slot.index] = std::move(value);
                return;
            }
            break;
        case Slot::Type::state:
            if (frame.instance != nullptr) {
                frame.instance->state = to_string(value);
                return;
            }
            break;
        default: break;
        }
    }
    error(where(frame) + ": cannot write argument " + std::to_string(arg));
}

std::string_view Vm::type_of(const Frame& frame, std::uint32_t arg) const {
    const Function& fn = *frame.fn;
    const Slot& slot = fn.slots[arg];
    if (fn.args[arg].type() != 1) {
        return {};
    }
    switch (slot.type) {
    case Slot::Type::reg: return fn.reg_types[slot.index];
    case Slot::Type::var: return fn.owner->variables()[slot.index].type;
    case Slot::Type::state: return "string";
    case Slot::Type::self: return fn.owner->key();
    default: return {};
    }
}

bool Vm::push(Thread& thread, const Function* fn, Instance* instance, Value self,
              std::vector<Value> args, int result_arg) {
    if (thread.frames.size() >= k_max_depth) {
        error(fn->owner->name() + "." + fn->name + ": stack overflow");
        return false;
    }
    Frame frame;
    frame.fn = fn;
    frame.instance = instance;
    frame.self = std::move(self);
    frame.result_arg = result_arg;
    frame.regs.reserve(fn->reg_names.size());
    for (std::size_t r = 0; r < fn->reg_names.size(); ++r) {
        if (r < fn->param_count && r < args.size()) {
            frame.regs.push_back(cast(args[r], fn->reg_types[r]));
        } else {
            frame.regs.push_back(default_for(fn->reg_types[r]));
        }
    }
    thread.frames.push_back(std::move(frame));
    return true;
}

void Vm::finish_frame(Thread& thread, Value result) {
    const int result_arg = thread.frames.back().result_arg;
    thread.frames.pop_back();
    if (thread.frames.empty()) {
        thread.done = true;
        thread.result = std::move(result);
        return;
    }
    Frame& caller = thread.frames.back();
    if (result_arg >= 0) {
        write(caller, static_cast<std::uint32_t>(result_arg), std::move(result));
    }
    ++caller.pc;
}

bool Vm::call_native(Thread& thread, const Function* fn, Value self, std::vector<Value> args,
                     int result_arg) {
    const auto key = fn->owner->key() + "." + fn->name;
    NativeResult result;
    if (const auto it = natives_.find(key); it != natives_.end()) {
        for (std::size_t p = 0; p < args.size() && p < fn->param_count; ++p) {
            args[p] = cast(args[p], fn->reg_types[p]);
        }
        NativeCall call{*this, thread, std::move(self), args};
        result = it->second(call);
    } else if (unbound_logged_.insert(key).second) {
        error("native " + fn->owner->name() + "." + fn->name + " is not bound");
    }
    Frame& frame = thread.frames.back();
    if (result_arg >= 0) {
        write(frame, static_cast<std::uint32_t>(result_arg), std::move(result.value));
    }
    ++frame.pc;
    if (!result.wait_for.empty()) {
        thread.wait_for = to_lower(result.wait_for);
        thread.wake_at = time_ + (result.wait >= 0.0 ? result.wait : 1.0e9);
        return false;
    }
    if (result.wait >= 0.0) {
        thread.wake_at = time_ + result.wait;
        return false;
    }
    return true;
}

void Vm::run(Thread& thread) {
    for (std::uint64_t n = 0; n < budget_; ++n) {
        if (thread.done || !step(thread)) {
            return;
        }
    }
}

bool Vm::step(Thread& thread) {
    Frame& fr = thread.frames.back();
    const Function& fn = *fr.fn;
    if (fr.pc >= fn.ops.size()) {
        finish_frame(thread, {});
        return !thread.done;
    }
    ++instructions_;
    const std::uint32_t a = fn.offsets[fr.pc];
    const std::uint32_t end = fn.offsets[fr.pc + 1];
    static const std::string k_no_name;
    const auto name_at = [&](std::uint32_t arg) -> const std::string& {
        const auto type = fn.args[arg].type();
        return type == 1 || type == 2 ? fn.owner->lower(fn.args[arg].data()) : k_no_name;
    };
    const auto rest = [&](std::uint32_t from) {
        std::vector<Value> out;
        for (std::uint32_t i = from; i < end; ++i) {
            out.push_back(read(fr, i));
        }
        return out;
    };
    const auto jump = [&](std::uint32_t arg) {
        fr.pc = static_cast<std::uint32_t>(static_cast<std::int64_t>(fr.pc) + as_int(read(fr, arg)));
    };
    const auto next = [&] {
        ++fr.pc;
        return true;
    };
    const auto int_op = [&](std::int32_t (*op)(std::int32_t, std::int32_t)) {
        write(fr, a, Value::integer(op(as_int(read(fr, a + 1)), as_int(read(fr, a + 2)))));
        return next();
    };

    switch (fn.ops[fr.pc]) {
    case Op::nop: return next();
    case Op::iadd: return int_op(int_add);
    case Op::fadd: write(fr, a, Value::floating(as_float(read(fr, a + 1)) + as_float(read(fr, a + 2)))); return next();
    case Op::isub: return int_op(int_sub);
    case Op::fsub: write(fr, a, Value::floating(as_float(read(fr, a + 1)) - as_float(read(fr, a + 2)))); return next();
    case Op::imul: return int_op(int_mul);
    case Op::fmul: write(fr, a, Value::floating(as_float(read(fr, a + 1)) * as_float(read(fr, a + 2)))); return next();
    case Op::idiv:
    case Op::imod: {
        const auto x = as_int(read(fr, a + 1));
        const auto y = as_int(read(fr, a + 2));
        if (y == 0) {
            error(where(fr) + ": integer division by zero");
            write(fr, a, Value::integer(0));
        } else {
            write(fr, a, Value::integer(fn.ops[fr.pc] == Op::idiv ? int_div(x, y) : int_mod(x, y)));
        }
        return next();
    }
    case Op::fdiv: {
        const auto y = as_float(read(fr, a + 2));
        if (y == 0.0F) {
            error(where(fr) + ": division by zero");
            write(fr, a, Value::floating(0.0F));
        } else {
            write(fr, a, Value::floating(as_float(read(fr, a + 1)) / y));
        }
        return next();
    }
    case Op::not_: write(fr, a, Value::boolean(!truthy(read(fr, a + 1)))); return next();
    case Op::ineg: write(fr, a, Value::integer(int_neg(as_int(read(fr, a + 1))))); return next();
    case Op::fneg: write(fr, a, Value::floating(-as_float(read(fr, a + 1)))); return next();
    case Op::assign: write(fr, a, read(fr, a + 1)); return next();
    case Op::cast: write(fr, a, cast(read(fr, a + 1), type_of(fr, a))); return next();
    case Op::cmp_eq: write(fr, a, Value::boolean(equals(read(fr, a + 1), read(fr, a + 2)))); return next();
    case Op::cmp_lt: write(fr, a, Value::boolean(order(read(fr, a + 1), read(fr, a + 2)) < 0)); return next();
    case Op::cmp_le: write(fr, a, Value::boolean(order(read(fr, a + 1), read(fr, a + 2)) <= 0)); return next();
    case Op::cmp_gt: write(fr, a, Value::boolean(order(read(fr, a + 1), read(fr, a + 2)) > 0)); return next();
    case Op::cmp_ge: write(fr, a, Value::boolean(order(read(fr, a + 1), read(fr, a + 2)) >= 0)); return next();
    case Op::jmp: jump(a); return true;
    case Op::jmpt:
        if (truthy(read(fr, a))) {
            jump(a + 1);
            return true;
        }
        return next();
    case Op::jmpf:
        if (!truthy(read(fr, a))) {
            jump(a + 1);
            return true;
        }
        return next();
    case Op::return_: finish_frame(thread, read(fr, a)); return !thread.done;
    case Op::strcat:
        write(fr, a, Value::string(to_string(read(fr, a + 1)) + to_string(read(fr, a + 2))));
        return next();

    case Op::callmethod: {
        const auto& name = name_at(a);
        Value object = read(fr, a + 1);
        if (object.is_none()) {
            error(where(fr) + ": cannot call " + name + " on None");
            write(fr, a + 2, {});
            return next();
        }
        const Found found = find_method(object, name);
        if (found.fn == nullptr) {
            // Missing events (OnBeginState and the like) are not errors.
            if (!starts_with_on(name)) {
                error(where(fr) + ": no function " + name + " on " + to_string(object));
            }
            write(fr, a + 2, {});
            return next();
        }
        Value self = found.instance != nullptr
                         ? Value::object(object.form, found.instance->cls, found.instance)
                         : object;
        if (found.fn->native) {
            return call_native(thread, found.fn, std::move(self), rest(a + 3), static_cast<int>(a + 2));
        }
        if (!push(thread, found.fn, found.instance, std::move(self), rest(a + 3), static_cast<int>(a + 2))) {
            write(fr, a + 2, {});
            return next();
        }
        return true;
    }
    case Op::callparent: {
        const auto& name = name_at(a);
        const auto* parent = fn.owner->parent();
        const auto* target = find_in_chain(parent, fr.instance != nullptr ? to_lower(fr.instance->state) : "", name);
        if (target == nullptr) {
            error(where(fr) + ": no parent function " + name);
            write(fr, a + 1, {});
            return next();
        }
        if (target->native) {
            return call_native(thread, target, fr.self, rest(a + 2), static_cast<int>(a + 1));
        }
        if (!push(thread, target, fr.instance, fr.self, rest(a + 2), static_cast<int>(a + 1))) {
            write(fr, a + 1, {});
            return next();
        }
        return true;
    }
    case Op::callstatic: {
        const auto* cls = load_class(name_at(a));
        const auto& name = name_at(a + 1);
        const auto* target = cls != nullptr ? find_in_chain(cls, "", name) : nullptr;
        if (target == nullptr) {
            error(where(fr) + ": no global function " + fn.owner->string(fn.args[a].data()) + "." +
                  fn.owner->string(fn.args[a + 1].data()));
            write(fr, a + 2, {});
            return next();
        }
        if (target->native) {
            return call_native(thread, target, {}, rest(a + 3), static_cast<int>(a + 2));
        }
        if (!push(thread, target, nullptr, {}, rest(a + 3), static_cast<int>(a + 2))) {
            write(fr, a + 2, {});
            return next();
        }
        return true;
    }

    case Op::propget:
    case Op::propset: {
        const bool get = fn.ops[fr.pc] == Op::propget;
        const auto& name = name_at(a);
        Value object = read(fr, a + 1);
        if (object.is_none()) {
            error(where(fr) + ": property " + name + " of None");
            if (get) {
                write(fr, a + 2, {});
            }
            return next();
        }
        // The attached script the object is seen as, else any derived one.
        std::vector<Instance*> candidates;
        if (object.instance != nullptr) {
            candidates.push_back(object.instance);
        }
        for (auto* i : instances(object.form)) {
            if (object.cls == nullptr || i->cls->derives_from(object.cls)) {
                candidates.push_back(i);
            }
        }
        for (auto* i : candidates) {
            for (const auto* c = i->cls; c != nullptr; c = c->parent()) {
                const auto* p = c->property(name);
                if (p == nullptr) {
                    continue;
                }
                const Value self = Value::object(object.form, i->cls, i);
                if (p->auto_var) {
                    auto& var = (*i->vars_of(c))[*p->auto_var];
                    if (get) {
                        write(fr, a + 2, var);
                    } else {
                        var = cast(read(fr, a + 2), p->type);
                    }
                    return next();
                }
                const Function* handler = get ? p->getter : p->setter;
                if (handler != nullptr &&
                    push(thread, handler, i, self, get ? std::vector<Value>{} : std::vector<Value>{read(fr, a + 2)},
                         get ? static_cast<int>(a + 2) : -1)) {
                    return true;
                }
                break;
            }
        }
        // A property with a getter or setter on the object's own class (as
        // GlobalVariable.Value) runs without an instance, like its methods.
        for (const auto* c = object.cls; c != nullptr; c = c->parent()) {
            const auto* p = c->property(name);
            if (p == nullptr) {
                continue;
            }
            const Function* handler = get ? p->getter : p->setter;
            if (!p->auto_var && handler != nullptr &&
                push(thread, handler, nullptr, object, get ? std::vector<Value>{} : std::vector<Value>{read(fr, a + 2)},
                     get ? static_cast<int>(a + 2) : -1)) {
                return true;
            }
            break;
        }
        error(where(fr) + ": no property " + name + " on " + to_string(object));
        if (get) {
            write(fr, a + 2, {});
        }
        return next();
    }

    case Op::array_create: {
        const auto type = type_of(fr, a);
        const auto size = as_int(read(fr, a + 1));
        if (size < 0 || size > k_max_array || !type.ends_with("[]")) {
            error(where(fr) + ": cannot create an array of " + std::to_string(size));
            write(fr, a, {});
            return next();
        }
        const auto element = type.substr(0, type.size() - 2);
        write(fr, a, Value::make_array(std::make_shared<Array>(static_cast<std::size_t>(size), default_for(element))));
        return next();
    }
    case Op::array_length: {
        const Value arr = read(fr, a + 1);
        write(fr, a, Value::integer(arr.kind == Kind::array ? static_cast<std::int32_t>(arr.array->size()) : 0));
        return next();
    }
    case Op::array_getelement: {
        const Value arr = read(fr, a + 1);
        const auto index = as_int(read(fr, a + 2));
        if (arr.kind != Kind::array || index < 0 || static_cast<std::size_t>(index) >= arr.array->size()) {
            error(where(fr) + ": array index " + std::to_string(index) + " out of range");
            write(fr, a, {});
        } else {
            write(fr, a, (*arr.array)[static_cast<std::size_t>(index)]);
        }
        return next();
    }
    case Op::array_setelement: {
        const Value arr = read(fr, a);
        const auto index = as_int(read(fr, a + 1));
        if (arr.kind != Kind::array || index < 0 || static_cast<std::size_t>(index) >= arr.array->size()) {
            error(where(fr) + ": array index " + std::to_string(index) + " out of range");
        } else if (Value element = read(fr, a + 2); element.kind == Kind::array) {
            // Papyrus has no arrays of arrays; one could hold itself, and
            // to_string would never return.
            error(where(fr) + ": an array cannot hold an array");
        } else {
            (*arr.array)[static_cast<std::size_t>(index)] = std::move(element);
        }
        return next();
    }
    case Op::array_findelement:
    case Op::array_rfindelement: {
        const Value arr = read(fr, a);
        const Value wanted = read(fr, a + 2);
        auto from = as_int(read(fr, a + 3));
        std::int32_t found = -1;
        if (arr.kind == Kind::array) {
            const auto size = static_cast<std::int32_t>(arr.array->size());
            if (fn.ops[fr.pc] == Op::array_findelement) {
                for (auto i = std::max(from, 0); i < size && found < 0; ++i) {
                    if (equals((*arr.array)[static_cast<std::size_t>(i)], wanted)) {
                        found = i;
                    }
                }
            } else {
                // A negative start means the last element.
                from = from < 0 || from >= size ? size - 1 : from;
                for (auto i = from; i >= 0 && found < 0; --i) {
                    if (equals((*arr.array)[static_cast<std::size_t>(i)], wanted)) {
                        found = i;
                    }
                }
            }
        }
        write(fr, a + 1, Value::integer(found));
        return next();
    }
    }
    return next();
}

} // namespace skydot::vm
